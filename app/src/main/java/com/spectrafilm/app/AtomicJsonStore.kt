/*
 * Spektrafilm for Android — atomic, bounded JSON persistence primitives. GPLv3.
 *
 * App-private recipes/presets use AndroidX AtomicFile: bytes are written to a
 * sibling, flushed + fsynced, and renamed only after the complete write succeeds.
 * Provider imports use the same strict bounded UTF-8 reader before org.json sees
 * the document, preventing an unbounded readBytes() allocation.
 */
package com.spectrafilm.app

import androidx.core.util.AtomicFile
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.CharBuffer
import java.nio.charset.CodingErrorAction
import java.math.BigDecimal
import java.math.BigInteger
import java.util.concurrent.ConcurrentHashMap
import org.json.JSONArray
import org.json.JSONObject

internal class DocumentLimitException(message: String) : IOException(message)

internal data class JsonStructureLimits(
    val maxDepth: Int = 32,
    val maxNodes: Int = 200_000,
    val maxArrayLength: Int = 4_096,
    val maxObjectKeys: Int = 2_048,
    val maxStringChars: Int = 65_536,
    val maxTokenChars: Int = 128,
    val maxInputChars: Int = 4 * 1024 * 1024,
) {
    init {
        require(maxDepth > 0)
        require(maxNodes > 0)
        require(maxArrayLength >= 0)
        require(maxObjectKeys >= 0)
        require(maxStringChars >= 0)
        require(maxTokenChars > 0)
        require(maxInputChars >= 0)
    }
}

/** Result of the bounded lexical pass; computing it does not materialize a JSON tree or byte array. */
internal data class JsonTextMeasurement(
    val nodeCount: Int,
    val maxDepth: Int,
    val utf8Bytes: Int,
)

internal data class MeasuredJsonObject(
    val value: JSONObject,
    val measurement: JsonTextMeasurement,
)

internal object AtomicJsonStore {
    const val MAX_PRESET_BYTES: Int = 4 * 1024 * 1024
    const val MAX_RECIPE_BYTES: Int = 4 * 1024 * 1024

    private val fileLocks = ConcurrentHashMap<String, Any>()
    private data class JsonShapeMeasurement(val nodeCount: Int, val maxDepth: Int)

    /**
     * Holds the same reentrant per-path lock used by read/write/delete/quarantine across a
     * multi-step classification transaction. This prevents a valid replacement from landing
     * between reading corrupt bytes and moving those bytes to quarantine.
     */
    fun <T> withPathLock(file: File, operation: () -> T): T {
        val lock = fileLocks.computeIfAbsent(file.absoluteFile.normalize().path) { Any() }
        return synchronized(lock, operation)
    }

    fun writeUtf8(file: File, text: String, maxBytes: Int) {
        val encoded = Charsets.UTF_8.newEncoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
            .encode(CharBuffer.wrap(text))
        val bytes = ByteArray(encoded.remaining()).also(encoded::get)
        if (bytes.size > maxBytes) {
            throw DocumentLimitException("document is ${bytes.size} bytes; limit is $maxBytes")
        }
        write(file, maxBytes) { output -> output.write(bytes) }
    }

    /** [writer] must not close [OutputStream]; AtomicFile owns close/sync/commit. */
    fun write(file: File, maxBytes: Int, writer: (OutputStream) -> Unit) {
        require(maxBytes >= 0) { "maxBytes must be non-negative" }
        withPathLock(file) {
            val parent = file.parentFile ?: throw IOException("document has no parent: $file")
            if (!parent.isDirectory && !parent.mkdirs()) {
                throw IOException("could not create document directory: $parent")
            }
            val atomic = AtomicFile(file)
            var stream: FileOutputStream? = null
            try {
                stream = atomic.startWrite()
                writer(LimitedOutputStream(stream, maxBytes.toLong()))
                atomic.finishWrite(stream)
                stream = null
            } catch (failure: Throwable) {
                if (stream != null) {
                    runCatching { atomic.failWrite(stream) }
                        .exceptionOrNull()
                        ?.let(failure::addSuppressed)
                }
                throw failure
            }
        }
    }

    fun readUtf8(file: File, maxBytes: Int): String {
        return withPathLock(file) {
            AtomicFile(file).openRead().use { readUtf8(it, maxBytes) }
        }
    }

    fun readUtf8(input: InputStream, maxBytes: Int): String {
        require(maxBytes >= 0) { "maxBytes must be non-negative" }
        val output = ByteArrayOutputStream(minOf(maxBytes, 16 * 1024))
        val buffer = ByteArray(8 * 1024)
        var total = 0
        while (true) {
            val count = input.read(buffer)
            if (count < 0) break
            total += count
            if (total > maxBytes) {
                throw DocumentLimitException("document exceeds $maxBytes bytes")
            }
            output.write(buffer, 0, count)
        }
        return Charsets.UTF_8.newDecoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
            .decode(ByteBuffer.wrap(output.toByteArray()))
            .toString()
    }

    /**
     * Parse an object only after a bounded, non-allocating lexical pass has proved that
     * nesting, collection sizes, strings, scalar tokens, and JSON syntax are acceptable.
     * This keeps an adversarial document away from JSONObject's recursive parser until
     * its maximum recursion and allocation shape is known.
     */
    fun parseObject(
        text: String,
        limits: JsonStructureLimits = JsonStructureLimits(),
    ): JSONObject = parseMeasuredObject(text, limits).value

    /**
     * Parse one already-bounded object and return the non-allocating preflight measurement that
     * admitted it. Callers may inspect the object and then discard it; no parsed-tree cache is held.
     */
    fun parseMeasuredObject(
        text: String,
        limits: JsonStructureLimits = JsonStructureLimits(),
    ): MeasuredJsonObject {
        val measurement = measureText(text, limits)
        // Android's platform JSONTokener narrows every non-Long number to Double. That can
        // round a token such as 2.0000000000000001 to exactly 2.0 before schema/version
        // validation sees it. Reparse the already bounded text while retaining integer and
        // decimal tokens as BigInteger/BigDecimal so device and JVM validation are identical.
        val value = ExactJsonParser(text).parseDocumentObject().also { validate(it, limits) }
        return MeasuredJsonObject(value, measurement)
    }

    /** Validate a complete RFC-8259 JSON value without constructing its object tree. */
    fun validateText(text: String, limits: JsonStructureLimits = JsonStructureLimits()) {
        measureText(text, limits)
    }

    /** Validate and measure a complete JSON value without constructing its object tree. */
    fun measureText(
        text: String,
        limits: JsonStructureLimits = JsonStructureLimits(),
    ): JsonTextMeasurement {
        val shape = JsonTextPreflight(text, limits).validateDocument()
        return JsonTextMeasurement(shape.nodeCount, shape.maxDepth, utf8Length(text))
    }

    /** Exact strict UTF-8 length without allocating the encoded byte array. */
    fun utf8Length(text: String): Int {
        var bytes = 0L
        var index = 0
        while (index < text.length) {
            val char = text[index]
            bytes += when {
                char.code <= 0x7f -> 1L
                char.code <= 0x7ff -> 2L
                Character.isHighSurrogate(char) &&
                    index + 1 < text.length &&
                    Character.isLowSurrogate(text[index + 1]) -> {
                    index++
                    4L
                }
                Character.isSurrogate(char) -> throw CharacterCodingException()
                else -> 3L
            }
            if (bytes > Int.MAX_VALUE) throw DocumentLimitException("UTF-8 length exceeds Int range")
            index++
        }
        return bytes.toInt()
    }

    /**
     * Truncate a well-formed UTF-16 string without splitting a supplementary code point. The
     * persisted contracts express limits in UTF-16 code units, so this preserves that limit while
     * refusing the replacement-character behavior of String.take()/the default UTF-8 encoder.
     */
    fun truncateUtf16Safely(text: String, maxChars: Int): String {
        require(maxChars >= 0) { "maxChars must be non-negative" }
        // Validate the complete producer value first; truncation must not hide malformed input in
        // the discarded suffix.
        utf8Length(text)
        if (text.length <= maxChars) return text
        val end = if (
            maxChars > 0 &&
            Character.isHighSurrogate(text[maxChars - 1]) &&
            Character.isLowSurrogate(text[maxChars])
        ) {
            maxChars - 1
        } else {
            maxChars
        }
        return text.substring(0, end)
    }

    /**
     * Delete every AtomicFile generation under the same per-path lock as read/write.
     * The boolean reports whether any generation existed before deletion; a leftover
     * base, .new, or legacy .bak is an IO failure rather than a silent partial delete.
     */
    fun delete(file: File): Boolean {
        return withPathLock(file) {
            val generations = listOf(file, File(file.path + ".new"), File(file.path + ".bak"))
            val existed = generations.any(File::exists)
            AtomicFile(file).delete()
            val leftovers = generations.filter(File::exists)
            if (leftovers.isNotEmpty()) {
                throw IOException(
                    "could not delete atomic document generations: " +
                        leftovers.joinToString { it.name },
                )
            }
            existed
        }
    }

    /**
     * Move a corrupt base document aside in the same directory. The sibling rename
     * preserves evidence for diagnostics while removing it from the active namespace.
     */
    fun quarantine(file: File, nowMillis: Long = System.currentTimeMillis()): File {
        return withPathLock(file) {
            // Let AtomicFile reconcile an interrupted .new/.bak before preserving the base.
            runCatching { AtomicFile(file).openRead().use { } }
            if (!file.isFile) throw IOException("document disappeared before quarantine: $file")
            var quarantine = File(file.parentFile, "${file.name}.corrupt-$nowMillis")
            var suffix = 1
            while (quarantine.exists()) {
                quarantine = File(file.parentFile, "${file.name}.corrupt-$nowMillis-${suffix++}")
            }
            if (!file.renameTo(quarantine)) {
                throw IOException("could not quarantine corrupt document: $file")
            }
            // A failed future write is never allowed to resurrect beside the quarantined base.
            val staleGenerations = listOf(File(file.path + ".new"), File(file.path + ".bak"))
            staleGenerations.forEach { generation ->
                if (generation.exists()) generation.delete()
            }
            val leftovers = staleGenerations.filter(File::exists)
            if (leftovers.isNotEmpty()) {
                throw IOException(
                    "quarantined document but could not remove stale generations: " +
                        leftovers.joinToString { it.name },
                )
            }
            quarantine
        }
    }

    fun validate(root: Any?, limits: JsonStructureLimits = JsonStructureLimits()) {
        var nodes = 0

        fun visit(value: Any?, depth: Int) {
            if (depth > limits.maxDepth) {
                throw DocumentLimitException("JSON depth exceeds ${limits.maxDepth}")
            }
            nodes++
            if (nodes > limits.maxNodes) {
                throw DocumentLimitException("JSON node count exceeds ${limits.maxNodes}")
            }
            when (value) {
                null, JSONObject.NULL, is Boolean -> Unit
                is Double -> if (!value.isFinite()) {
                    throw DocumentLimitException("JSON number must be finite")
                }
                is Float -> if (!value.isFinite()) {
                    throw DocumentLimitException("JSON number must be finite")
                }
                is Number -> Unit
                is String -> if (value.length > limits.maxStringChars) {
                    throw DocumentLimitException("JSON string exceeds ${limits.maxStringChars} characters")
                } else utf8Length(value)
                is JSONArray -> {
                    if (value.length() > limits.maxArrayLength) {
                        throw DocumentLimitException("JSON array length exceeds ${limits.maxArrayLength}")
                    }
                    for (index in 0 until value.length()) visit(value.opt(index), depth + 1)
                }
                is JSONObject -> {
                    if (value.length() > limits.maxObjectKeys) {
                        throw DocumentLimitException("JSON object key count exceeds ${limits.maxObjectKeys}")
                    }
                    val keys = value.keys()
                    while (keys.hasNext()) {
                        val key = keys.next()
                        if (key.length > limits.maxStringChars) {
                            throw DocumentLimitException("JSON key exceeds ${limits.maxStringChars} characters")
                        }
                        utf8Length(key)
                        visit(value.opt(key), depth + 1)
                    }
                }
                else -> throw DocumentLimitException("unsupported JSON value: ${value.javaClass.name}")
            }
        }

        visit(root, 1)
    }

    /** Strict RFC-8259 object parser used only after [JsonTextPreflight] has bounded the input. */
    private class ExactJsonParser(private val text: String) {
        private var position = 0

        fun parseDocumentObject(): JSONObject {
            skipWhitespace()
            val root = parseValue()
            skipWhitespace()
            check(position == text.length) { "preflight/parser disagreement: trailing content" }
            return root as? JSONObject
                ?: throw IllegalArgumentException("JSON document must be an object")
        }

        private fun parseValue(): Any = when (peek()) {
            '{' -> parseObject()
            '[' -> parseArray()
            '"' -> parseString()
            't' -> parseLiteral("true", true)
            'f' -> parseLiteral("false", false)
            'n' -> parseLiteral("null", JSONObject.NULL)
            '-', in '0'..'9' -> parseNumber()
            else -> error("preflight/parser disagreement at character $position")
        }

        private fun parseObject(): JSONObject {
            expect('{')
            skipWhitespace()
            val result = JSONObject()
            if (consume('}')) return result
            val keys = HashSet<String>()
            while (true) {
                val key = parseString()
                if (!keys.add(key)) {
                    throw IllegalArgumentException("duplicate JSON object key")
                }
                skipWhitespace()
                expect(':')
                skipWhitespace()
                result.put(key, parseValue())
                skipWhitespace()
                if (consume('}')) return result
                expect(',')
                skipWhitespace()
            }
        }

        private fun parseArray(): JSONArray {
            expect('[')
            skipWhitespace()
            val result = JSONArray()
            if (consume(']')) return result
            while (true) {
                result.put(parseValue())
                skipWhitespace()
                if (consume(']')) return result
                expect(',')
                skipWhitespace()
            }
        }

        private fun parseString(): String {
            expect('"')
            val result = StringBuilder()
            while (true) {
                val value = text[position++]
                when (value) {
                    '"' -> return result.toString()
                    '\\' -> when (val escaped = text[position++]) {
                        '"', '\\', '/' -> result.append(escaped)
                        'b' -> result.append('\b')
                        'f' -> result.append('\u000c')
                        'n' -> result.append('\n')
                        'r' -> result.append('\r')
                        't' -> result.append('\t')
                        'u' -> {
                            val codeUnit = text.substring(position, position + 4).toInt(16)
                            result.append(codeUnit.toChar())
                            position += 4
                        }
                        else -> error("preflight/parser disagreement: invalid escape $escaped")
                    }
                    else -> result.append(value)
                }
            }
        }

        private fun parseNumber(): Number {
            val start = position
            while (position < text.length && !isDelimiter(text[position])) position++
            val token = text.substring(start, position)
            return if (token.indexOf('.') >= 0 || token.indexOf('e', ignoreCase = true) >= 0) {
                BigDecimal(token)
            } else {
                BigInteger(token)
            }
        }

        private fun parseLiteral(expected: String, value: Any): Any {
            check(text.regionMatches(position, expected, 0, expected.length)) {
                "preflight/parser disagreement: invalid literal"
            }
            position += expected.length
            return value
        }

        private fun skipWhitespace() {
            while (position < text.length && text[position] in JSON_WHITESPACE) position++
        }

        private fun peek(): Char = text[position]

        private fun consume(expected: Char): Boolean {
            if (position >= text.length || text[position] != expected) return false
            position++
            return true
        }

        private fun expect(expected: Char) {
            check(consume(expected)) {
                "preflight/parser disagreement: expected '$expected' at character $position"
            }
        }

        private fun isDelimiter(value: Char): Boolean =
            value in JSON_WHITESPACE || value == ',' || value == ']' || value == '}'

        private companion object {
            val JSON_WHITESPACE = charArrayOf(' ', '\t', '\r', '\n')
        }
    }

    private class JsonTextPreflight(
        private val text: String,
        private val limits: JsonStructureLimits,
    ) {
        private var position = 0
        private var nodes = 0
        private var maximumDepth = 0

        fun validateDocument(): JsonShapeMeasurement {
            limit(
                text.length <= limits.maxInputChars,
                "JSON input exceeds ${limits.maxInputChars} characters",
            )
            skipWhitespace()
            parseValue(depth = 1)
            skipWhitespace()
            syntax(position == text.length, "trailing content")
            return JsonShapeMeasurement(nodes, maximumDepth)
        }

        private fun parseValue(depth: Int) {
            limit(depth <= limits.maxDepth, "JSON depth exceeds ${limits.maxDepth}")
            maximumDepth = maxOf(maximumDepth, depth)
            nodes++
            limit(nodes <= limits.maxNodes, "JSON node count exceeds ${limits.maxNodes}")
            syntax(position < text.length, "expected a JSON value")
            when (text[position]) {
                '{' -> parseObject(depth)
                '[' -> parseArray(depth)
                '"' -> parseString()
                't' -> parseLiteral("true")
                'f' -> parseLiteral("false")
                'n' -> parseLiteral("null")
                '-', in '0'..'9' -> parseNumber()
                else -> syntax(false, "unexpected character '${text[position]}'")
            }
        }

        private fun parseObject(depth: Int) {
            position++
            skipWhitespace()
            if (consume('}')) return
            var keys = 0
            while (true) {
                syntax(position < text.length && text[position] == '"', "expected an object key")
                parseString()
                keys++
                limit(keys <= limits.maxObjectKeys, "JSON object key count exceeds ${limits.maxObjectKeys}")
                skipWhitespace()
                syntax(consume(':'), "expected ':' after object key")
                skipWhitespace()
                parseValue(depth + 1)
                skipWhitespace()
                if (consume('}')) return
                syntax(consume(','), "expected ',' or '}'")
                skipWhitespace()
            }
        }

        private fun parseArray(depth: Int) {
            position++
            skipWhitespace()
            if (consume(']')) return
            var elements = 0
            while (true) {
                elements++
                limit(elements <= limits.maxArrayLength, "JSON array length exceeds ${limits.maxArrayLength}")
                parseValue(depth + 1)
                skipWhitespace()
                if (consume(']')) return
                syntax(consume(','), "expected ',' or ']'")
                skipWhitespace()
            }
        }

        private fun parseString() {
            syntax(consume('"'), "expected a string")
            var decodedChars = 0
            var pendingHighSurrogate = false
            while (position < text.length) {
                val value = text[position++]
                when {
                    value == '"' -> {
                        syntax(!pendingHighSurrogate, "unpaired high surrogate in string")
                        return
                    }
                    value == '\\' -> {
                        syntax(position < text.length, "unterminated string escape")
                        when (text[position++]) {
                            '"', '\\', '/', 'b', 'f', 'n', 'r', 't' ->
                                syntax(!pendingHighSurrogate, "unpaired high surrogate in string")
                            'u' -> {
                                syntax(position + 4 <= text.length, "truncated unicode escape")
                                var codeUnit = 0
                                repeat(4) {
                                    val digit = text[position++].digitToIntOrNull(16)
                                    syntax(digit != null, "invalid unicode escape")
                                    codeUnit = (codeUnit shl 4) or requireNotNull(digit)
                                }
                                val escaped = codeUnit.toChar()
                                when {
                                    Character.isHighSurrogate(escaped) -> {
                                        syntax(!pendingHighSurrogate, "unpaired high surrogate in string")
                                        pendingHighSurrogate = true
                                    }
                                    Character.isLowSurrogate(escaped) -> {
                                        syntax(pendingHighSurrogate, "unpaired low surrogate in string")
                                        pendingHighSurrogate = false
                                    }
                                    else -> syntax(
                                        !pendingHighSurrogate,
                                        "unpaired high surrogate in string",
                                    )
                                }
                            }
                            else -> syntax(false, "invalid string escape")
                        }
                        decodedChars++
                    }
                    value.code < 0x20 -> syntax(false, "unescaped control character in string")
                    Character.isHighSurrogate(value) -> {
                        syntax(!pendingHighSurrogate, "unpaired high surrogate in string")
                        pendingHighSurrogate = true
                        decodedChars++
                    }
                    Character.isLowSurrogate(value) -> {
                        syntax(pendingHighSurrogate, "unpaired low surrogate in string")
                        pendingHighSurrogate = false
                        decodedChars++
                    }
                    else -> {
                        syntax(!pendingHighSurrogate, "unpaired high surrogate in string")
                        decodedChars++
                    }
                }
                limit(
                    decodedChars <= limits.maxStringChars,
                    "JSON string exceeds ${limits.maxStringChars} characters",
                )
            }
            syntax(false, "unterminated string")
        }

        private fun parseLiteral(expected: String) {
            limit(expected.length <= limits.maxTokenChars, "JSON token exceeds ${limits.maxTokenChars} characters")
            syntax(
                position + expected.length <= text.length &&
                    text.regionMatches(position, expected, 0, expected.length),
                "invalid JSON literal",
            )
            position += expected.length
            syntax(isDelimiter(position), "invalid character after JSON literal")
        }

        private fun parseNumber() {
            val start = position
            consume('-')
            syntax(position < text.length, "truncated JSON number")
            if (consume('0')) {
                syntax(position >= text.length || text[position] !in '0'..'9', "leading zero in JSON number")
            } else {
                syntax(position < text.length && text[position] in '1'..'9', "invalid JSON number")
                while (position < text.length && text[position] in '0'..'9') advanceNumber(start)
            }
            if (consume('.')) {
                syntax(position < text.length && text[position] in '0'..'9', "missing fraction digits")
                while (position < text.length && text[position] in '0'..'9') advanceNumber(start)
            }
            if (position < text.length && (text[position] == 'e' || text[position] == 'E')) {
                advanceNumber(start)
                if (position < text.length && (text[position] == '+' || text[position] == '-')) {
                    advanceNumber(start)
                }
                syntax(position < text.length && text[position] in '0'..'9', "missing exponent digits")
                while (position < text.length && text[position] in '0'..'9') advanceNumber(start)
            }
            limit(position - start <= limits.maxTokenChars, "JSON token exceeds ${limits.maxTokenChars} characters")
            syntax(isDelimiter(position), "invalid character after JSON number")
        }

        private fun advanceNumber(start: Int) {
            position++
            limit(position - start <= limits.maxTokenChars, "JSON token exceeds ${limits.maxTokenChars} characters")
        }

        private fun skipWhitespace() {
            while (position < text.length && text[position] in JSON_WHITESPACE) position++
        }

        private fun consume(expected: Char): Boolean {
            if (position >= text.length || text[position] != expected) return false
            position++
            return true
        }

        private fun isDelimiter(index: Int): Boolean =
            index >= text.length || text[index] in JSON_WHITESPACE || text[index] == ',' ||
                text[index] == ']' || text[index] == '}'

        private fun limit(condition: Boolean, message: String) {
            if (!condition) throw DocumentLimitException(message)
        }

        private fun syntax(condition: Boolean, message: String) {
            if (!condition) throw IllegalArgumentException("invalid JSON at character $position: $message")
        }

        private companion object {
            val JSON_WHITESPACE = charArrayOf(' ', '\t', '\r', '\n')
        }
    }

    private class LimitedOutputStream(
        private val delegate: OutputStream,
        private val limit: Long,
    ) : OutputStream() {
        private var count = 0L

        init {
            require(limit >= 0) { "limit must be non-negative" }
        }

        override fun write(value: Int) {
            reserve(1)
            delegate.write(value)
        }

        override fun write(buffer: ByteArray, offset: Int, length: Int) {
            if (offset < 0 || length < 0 || offset > buffer.size - length) {
                throw IndexOutOfBoundsException()
            }
            reserve(length)
            delegate.write(buffer, offset, length)
        }

        override fun flush() = delegate.flush()

        private fun reserve(length: Int) {
            val requested = length.toLong()
            if (count > limit - requested) {
                throw DocumentLimitException("document exceeds $limit bytes")
            }
            count += requested
        }
    }
}
