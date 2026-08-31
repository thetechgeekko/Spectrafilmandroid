/*
 * Spektrafilm for Android — atomic, bounded JSON persistence tests. GPLv3.
 */
package com.spectrafilm.app

import java.io.ByteArrayInputStream
import java.io.IOException
import java.math.BigDecimal
import java.math.BigInteger
import java.nio.charset.CharacterCodingException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class AtomicJsonStoreTest {

    @get:Rule
    val temporary = TemporaryFolder()

    @Test
    fun interruptedReplace_preservesPreviouslyCommittedDocument() {
        val target = temporary.newFolder("recipes").resolve("recipe.json")
        AtomicJsonStore.writeUtf8(target, "{\"version\":1,\"value\":\"old\"}", 1024)

        expectThrows<IOException> {
            AtomicJsonStore.write(target, 1024) { output ->
                output.write("{\"version\":2,".toByteArray(Charsets.UTF_8))
                throw IOException("simulated interrupted write")
            }
        }

        assertEquals(
            "{\"version\":1,\"value\":\"old\"}",
            AtomicJsonStore.readUtf8(target, 1024),
        )
    }

    @Test
    fun oversizedReplacement_isRejectedWithoutClobberingOldDocument() {
        val target = temporary.newFolder("presets").resolve("preset.json")
        AtomicJsonStore.writeUtf8(target, "{\"version\":1}", 1024)

        expectThrows<DocumentLimitException> {
            AtomicJsonStore.writeUtf8(target, "x".repeat(33), 32)
        }

        assertEquals("{\"version\":1}", AtomicJsonStore.readUtf8(target, 1024))
    }

    @Test
    fun limitedOutputStream_rejectsOverflowedRangeWithoutConsumingByteBudget() {
        val target = temporary.newFolder("range-overflow").resolve("document.json")

        AtomicJsonStore.write(target, maxBytes = 1) { output ->
            expectThrows<IndexOutOfBoundsException> {
                output.write(byteArrayOf(0), Int.MAX_VALUE, 2)
            }
            output.write(42)
        }

        assertEquals(listOf(42.toByte()), target.readBytes().toList())
    }

    @Test
    fun boundedInput_stopsAtLimitInsteadOfAllocatingWholeProviderStream() {
        val input = ByteArrayInputStream(ByteArray(65) { 'x'.code.toByte() })
        expectThrows<DocumentLimitException> {
            AtomicJsonStore.readUtf8(input, 64)
        }
    }

    @Test
    fun malformedUtf8_isRejectedStrictly() {
        val malformed = byteArrayOf(0x7b, 0x22, 0x78, 0x22, 0x3a, 0x22, 0xc3.toByte(), 0x28, 0x22, 0x7d)
        expectThrows<CharacterCodingException> {
            AtomicJsonStore.readUtf8(ByteArrayInputStream(malformed), 1024)
        }
    }

    @Test
    fun corruptDocument_isQuarantinedUnderDeterministicSiblingName() {
        val target = temporary.newFolder("recipes").resolve("recipe.json")
        target.writeText("not json")
        target.resolveSibling(target.name + ".new").writeText("interrupted")

        val quarantined = AtomicJsonStore.quarantine(target, nowMillis = 1234L)

        assertFalse(target.exists())
        assertTrue(quarantined.isFile)
        assertEquals("recipe.json.corrupt-1234", quarantined.name)
        assertEquals("not json", quarantined.readText())
        assertFalse(target.resolveSibling(target.name + ".new").exists())
        assertFalse(target.resolveSibling(target.name + ".bak").exists())
    }

    @Test
    fun structuralLimits_rejectDeepWideAndOversizedDocuments() {
        val limits = JsonStructureLimits(
            maxDepth = 3,
            maxNodes = 8,
            maxArrayLength = 2,
            maxObjectKeys = 3,
            maxStringChars = 4,
        )

        expectThrows<DocumentLimitException> {
            AtomicJsonStore.validate(JSONObject("""{"a":{"b":{"c":{"d":1}}}}"""), limits)
        }
        expectThrows<DocumentLimitException> {
            AtomicJsonStore.validate(JSONArray("[1,2,3]"), limits)
        }
        expectThrows<DocumentLimitException> {
            AtomicJsonStore.validate(JSONObject("""{"value":"12345"}"""), limits)
        }
    }

    @Test
    fun structuralLimits_acceptBoundedRecipeShape() {
        val root = JSONObject().apply {
            put("version", 2)
            put("params", JSONObject().put("film", "x"))
            put("masks", JSONArray().put(JSONObject().put("id", 1)))
        }

        AtomicJsonStore.validate(root, JsonStructureLimits())
    }

    @Test
    fun lexicalPreflight_rejectsDeepNestingBeforeObjectParsing() {
        val text = "{\"value\":" + "[".repeat(10_000) + "0" + "]".repeat(10_000) + "}"

        expectThrows<DocumentLimitException> {
            AtomicJsonStore.parseObject(text, JsonStructureLimits(maxDepth = 8))
        }
    }

    @Test
    fun lexicalPreflight_rejectsOversizedDecodedStringsAndScalarTokens() {
        val limits = JsonStructureLimits(maxStringChars = 4, maxTokenChars = 8)

        expectThrows<DocumentLimitException> {
            AtomicJsonStore.parseObject("""{"value":"12345"}""", limits)
        }
        expectThrows<DocumentLimitException> {
            AtomicJsonStore.parseObject("""{"value":123456789}""", limits)
        }
    }

    @Test
    fun lexicalPreflight_acceptsEscapedStringAtDecodedCharacterLimit() {
        val root = AtomicJsonStore.parseObject(
            "{\"value\":\"a\\u0062\\n\"}",
            JsonStructureLimits(maxStringChars = 5),
        )

        assertEquals("ab\n", root.getString("value"))
    }

    @Test
    fun directParser_enforcesInclusiveMaximumInputCharacterContract() {
        val text = """{"value":1}"""

        assertEquals(
            BigInteger.ONE,
            AtomicJsonStore.parseObject(
                text,
                JsonStructureLimits(maxInputChars = text.length),
            ).opt("value"),
        )
        expectThrows<DocumentLimitException> {
            AtomicJsonStore.parseObject(
                "$text ",
                JsonStructureLimits(maxInputChars = text.length),
            )
        }
    }

    @Test
    fun exactParser_preservesDecimalAndIntegerTokensWithoutDoubleRounding() {
        val root = AtomicJsonStore.parseObject(
            """{"version":2.0000000000000001,"nested":{"timestamp":9223372036854775808}}""",
        )

        assertEquals(BigDecimal("2.0000000000000001"), root.opt("version"))
        assertEquals(
            BigInteger("9223372036854775808"),
            root.getJSONObject("nested").opt("timestamp"),
        )
    }

    @Test
    fun exactParser_rejectsDecodedEquivalentDuplicateObjectKeysAtEveryDepth() {
        for (text in listOf(
            """{"name":1,"name":2}""",
            """{"name":1,"n\u0061me":2}""",
            """{"outer":{"value":1,"value":2}}""",
        )) {
            expectThrows<IllegalArgumentException> {
                AtomicJsonStore.parseObject(text)
            }
        }
    }

    @Test
    fun structuralValidation_rejectsNonFiniteRuntimeNumbers() {
        expectThrows<DocumentLimitException> { AtomicJsonStore.validate(Double.NaN) }
        expectThrows<DocumentLimitException> { AtomicJsonStore.validate(Double.POSITIVE_INFINITY) }
        expectThrows<DocumentLimitException> { AtomicJsonStore.validate(Float.NEGATIVE_INFINITY) }
    }

    @Test
    fun lockedDelete_removesEveryAtomicGenerationAndReportsExistence() {
        val target = temporary.newFolder("delete").resolve("preset.json")
        target.writeText("base")
        target.resolveSibling(target.name + ".new").writeText("new")
        target.resolveSibling(target.name + ".bak").writeText("backup")

        assertTrue(AtomicJsonStore.delete(target))
        assertFalse(target.exists())
        assertFalse(target.resolveSibling(target.name + ".new").exists())
        assertFalse(target.resolveSibling(target.name + ".bak").exists())
        assertFalse(AtomicJsonStore.delete(target))
    }

    @Test
    fun classificationLock_quarantinesObservedCorruptionBeforeValidReplacementCanCommit() {
        val target = temporary.newFolder("classification-race").resolve("preset.json")
        AtomicJsonStore.writeUtf8(target, "corrupt-old-bytes", 1024)
        val classificationEntered = CountDownLatch(1)
        val releaseClassification = CountDownLatch(1)
        val writerAttempted = CountDownLatch(1)
        val writerCompleted = CountDownLatch(1)
        val failure = AtomicReference<Throwable?>(null)

        val classifier = Thread {
            try {
                AtomicJsonStore.withPathLock(target) {
                    assertEquals("corrupt-old-bytes", AtomicJsonStore.readUtf8(target, 1024))
                    classificationEntered.countDown()
                    assertTrue(releaseClassification.await(5, TimeUnit.SECONDS))
                    AtomicJsonStore.quarantine(target, nowMillis = 4321L)
                }
            } catch (caught: Throwable) {
                failure.compareAndSet(null, caught)
            }
        }
        val writer = Thread {
            try {
                assertTrue(classificationEntered.await(5, TimeUnit.SECONDS))
                writerAttempted.countDown()
                AtomicJsonStore.writeUtf8(target, "{\"version\":2}", 1024)
                writerCompleted.countDown()
            } catch (caught: Throwable) {
                failure.compareAndSet(null, caught)
            }
        }
        classifier.start()
        writer.start()
        assertTrue(writerAttempted.await(5, TimeUnit.SECONDS))
        assertFalse("replacement overtook classification", writerCompleted.await(250, TimeUnit.MILLISECONDS))
        releaseClassification.countDown()
        classifier.join(5_000)
        writer.join(5_000)

        failure.get()?.let { throw AssertionError("worker failed", it) }
        assertFalse(classifier.isAlive)
        assertFalse(writer.isAlive)
        assertEquals("{\"version\":2}", AtomicJsonStore.readUtf8(target, 1024))
        assertEquals(
            "corrupt-old-bytes",
            target.resolveSibling("${target.name}.corrupt-4321").readText(),
        )
    }

    private inline fun <reified T : Throwable> expectThrows(block: () -> Unit): T {
        try {
            block()
            fail("Expected ${T::class.java.simpleName}")
        } catch (failure: Throwable) {
            if (failure !is T) throw failure
            return failure
        }
        error("unreachable")
    }
}
