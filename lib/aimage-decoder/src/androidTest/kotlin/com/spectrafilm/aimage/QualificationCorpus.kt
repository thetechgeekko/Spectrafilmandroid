/*
 * Spektrafilm for Android — pinned AImageDecoder qualification corpus.
 * GPL-3.0-only. External Android CTS fixtures retain their upstream license.
 */
package com.spectrafilm.aimage

import android.util.Base64
import java.security.MessageDigest

internal data class QualificationFixture(
    val name: String,
    val mime: String,
    val kind: AImageInputKind,
    val expectedSha256: String,
    /** 0 means unknown/not applicable; 1..8 are TIFF/EXIF orientation values. */
    val encodedOrientation: Int,
    val provenance: String,
    val bytes: ByteArray,
    val displayReferredDngFallback: Boolean = false,
) {
    init {
        require(expectedSha256.matches(Regex("[0-9a-f]{64}")))
        require(encodedOrientation in 0..8)
        require(bytes.isNotEmpty() && bytes.size <= AIMAGE_MAX_ENCODED_BYTES)
        require(sha256(bytes) == expectedSha256) {
            "$name source digest differs from the pinned corpus"
        }
    }
}

internal data class ExternalFixtureSpec(
    val fileName: String,
    val mime: String,
    val kind: AImageInputKind,
    val expectedSha256: String,
    val encodedOrientation: Int = 0,
    val displayReferredDngFallback: Boolean = false,
    val hostileOnly: Boolean = false,
    val upstreamUrl: String,
)

/**
 * Tiny project-authored fixtures keep the preliminary runner completely offline.
 * They are deliberately not a performance corpus; the full CTS corpus below is
 * mandatory before an adoption decision.
 */
internal object QualificationCorpus {
    val embedded: List<QualificationFixture> = listOf(
        fixture(
            name = "project-gradient.jpg",
            mime = "image/jpeg",
            kind = AImageInputKind.JPEG,
            sha256 = "b7be17a857ffaf82258716d33628fa8aa1eaca718dbec6afa32d36699ed8b2b5",
            base64 = "/9j/4AAQSkZJRgABAgAAAQABAAD//gAQTGF2YzYyLjI4LjEwMQD/2wBDAAgEBAQEBAUFBQUFBQYGBgYGBgYGBgYGBgYHBwcICAgHBwcGBgcHCAgICAkJCQgICAgJCQoKCgwMCwsODg4RERT/xABpAAEBAAAAAAAAAAAAAAAAAAAFBgEBAQAAAAAAAAAAAAAAAAAABQYQAAICAgICAQUBAQAAAAAAAAEDAgQREgYTBQAVUUEzIiQhFhEAAwACAgMBAQEAAAAAAAAAAwECBAUTBiEVEgAiFP/AABEIAAwAEAMBIgACEQADEQD/2gAMAwEAAhEDEQA/AInjXJuZ+J5RcVx610SUuLpkpoyWpUK0jNjWXFSWuA7JAyZIDMgM5x6VxH/u+O+dkil/NZNSF/tHx7qpplLSLYtM7KZr6zMe4N69zpnf/PX+IIVadyyu6O6nXOKKZHJjtCdrWQzEiQyCRkEH6euUfF0V8J+KinFOPn30op3YcVxyqagvcz7Px/rttv8AfOfQtX3DJ6veeHChA9heowcpiVDZl/nssGNQjD5uFltSC58/TaLHlOm7T1oWw3/Zskt1byvcEMnSXKPjrW1jV9CJ/F4wpGyU7SEkPicr9//Z",
        ),
        fixture(
            name = "project-gradient-orientation-6.jpg",
            mime = "image/jpeg",
            kind = AImageInputKind.JPEG,
            sha256 = "b1d8e87f92617bb159bed0b32673859d4709ec8db333133f7d6c3cb5a26b7f2a",
            orientation = 6,
            base64 = "/9j/4QAiRXhpZgAASUkqAAgAAAABABIBAwABAAAABgAAAAAAAAD/4AAQSkZJRgABAgAAAQABAAD//gAQTGF2YzYyLjI4LjEwMQD/2wBDAAgEBAQEBAUFBQUFBQYGBgYGBgYGBgYGBgYHBwcICAgHBwcGBgcHCAgICAkJCQgICAgJCQoKCgwMCwsODg4RERT/xABpAAEBAAAAAAAAAAAAAAAAAAAFBgEBAQAAAAAAAAAAAAAAAAAABQYQAAICAgICAQUBAQAAAAAAAAEDAgQREgYTBQAVUUEzIiQhFhEAAwACAgMBAQEAAAAAAAAAAwECBAUTBiEVEgAiFP/AABEIAAwAEAMBIgACEQADEQD/2gAMAwEAAhEDEQA/AInjXJuZ+J5RcVx610SUuLpkpoyWpUK0jNjWXFSWuA7JAyZIDMgM5x6VxH/u+O+dkil/NZNSF/tHx7qpplLSLYtM7KZr6zMe4N69zpnf/PX+IIVadyyu6O6nXOKKZHJjtCdrWQzEiQyCRkEH6euUfF0V8J+KinFOPn30op3YcVxyqagvcz7Px/rttv8AfOfQtX3DJ6veeHChA9heowcpiVDZl/nssGNQjD5uFltSC58/TaLHlOm7T1oWw3/Zskt1byvcEMnSXKPjrW1jV9CJ/F4wpGyU7SEkPicr9//Z",
        ),
        fixture(
            name = "project-gradient.png",
            mime = "image/png",
            kind = AImageInputKind.PNG,
            sha256 = "be63cc25e26f6b1a328a999ad8ba7a84f65b373693378873a1d37015f217c29d",
            base64 = "iVBORw0KGgoAAAANSUhEUgAAABAAAAAMCAIAAADkharWAAAACXBIWXMAAAABAAAAAQBPJcTWAAAAu0lEQVR4nGP8y4AAzP9A5N//YDaEAwb/GEBCTP+ZGBgYWBhIBCwQyhFCOYCJ/0gcMLAH28Bgz0iBDfTSoPwZRK4MFQeFles3BgYG87obDAwMQgyg8LnlLYPFhqKiMIbHBxcseHnm1suJTSYTuybml+UzMDDU1dQxMMybOvUaiobbfgwMd1bZ72NgEGc4fMtuXR3Dv/rk4HqIZAnTVjsGBTtqeNrwI0NCgt4PL638yhnI4h6vD2Vnh5BsAwB/VzZAWrVBPwAAAABJRU5ErkJggg==",
        ),
        fixture(
            name = "project-alpha.png",
            mime = "image/png",
            kind = AImageInputKind.PNG,
            sha256 = "c2873e7844f4b0119105dc6cec43a4e1ba6601a12a0731be49fd253ffc5bd7e9",
            base64 = "iVBORw0KGgoAAAANSUhEUgAAABAAAAAMCAYAAABr5z2BAAAAAXNSR0IArs4c6QAAAARnQU1BAACxjwv8YQUAAAAJcEhZcwAADsMAAA7DAcdvqGQAAAIjSURBVDhPFc8hqoZAGAXQuwWT3QVY3IIuwfKwahYmGCxfErEZBcskwWgaELtBMJg1DlhMVrmP/+zgACAcvOLhZoALIQ6JsTLDggKT1BjYoceIVmZU3FDiRC4PUhJ/AFyK47703BuBe0noHozdFZm7SOFOrN0BndvL6Lac3QqbW8rp5nzcFHT/BPBJx3/h+bcE/sXQPxD7q2T+wsKfUPuDdH7P0W8x+5VsfsnTz/H4qdD/IxARTvSKF90MogthdEgcrcyiBUU0SR0N7KIeY9TKHFXcohJnlMsTpWT0KyQUJ3npJTeC5JIwORgnK7JkkSKZWCcDuqSXMWk5JxW2pJQzyfkkKZj8Cop01AtP3RKoi6E6EKtVMrWwUBNqNUineo6qxawq2VTJU+V4VCpUv0JDOM0rXnMzaC6EzSFxszJrFhTNJHUzsGt6jE0rc1Nxa0qcTS5Pk5LNr6Apjn7p6RuBviTUB2O9ItOLFHpirQd0updRt5x1hU2Xcuqcj05B/SsY0jEvPHNLYC6G5kBsVsnMwsJMqM0gnek5mhazqWQzJU+T4zGp0PwKO+Hsr3j7zWC/EO6HxPvKbF9Q7JPU+8Bu7zHurcx7xW0vce65PHtK7r+CpTj2pWdvBPaS0B6M7YrMLlLYibUd0NleRttythU2W8ppcz42Be2v8JHO98L7bgm+i+F3IP5Wyb6FxTeh/gbpvp7j12L+Ktm+kueX4/lS4ffHf44un7DdNWk1AAAAAElFTkSuQmCC",
        ),
        // Exact project-authored 4x2 RGBA16 oracle. Its eight pixels are
        // deliberately off the 8-bit lattice and include alpha=0 with hidden
        // RGB, so an RGBA_F16 decode can be checked against source samples
        // instead of merely checking format metadata or a self-digest.
        fixture(
            name = "project-rgba16-oracle.png",
            mime = "image/png",
            kind = AImageInputKind.PNG,
            sha256 = "50a5c017735f446159a4b956c1042765d032f922d9bac3862cce0f249cb7af04",
            base64 = "iVBORw0KGgoAAAANSUhEUgAAAAQAAAACEAYAAAAvOKEgAAAAS0lEQVR42gXBsRVAQABEwV0EMlQglJ5AIKIKnYjdU8e1oBWRIq4JvhnJNh+UeWjTKkVFxyIKwvyMoMKVa0NDz8LBdp/TleQ3q9sD/FYeHB/AWw7+AAAAAElFTkSuQmCC",
        ),
        fixture(
            name = "project-first-frame.gif",
            mime = "image/gif",
            kind = AImageInputKind.GIF,
            sha256 = "3415b750fc68832d55cf9891a8bb9dea031784a2748a23ef4acc58a05dfa500c",
            base64 = "R0lGODlhEAAMAPcfMQAAACQAAEgAAGwAAJAAALQAANgAAPwAAAAkACQkAEgkAGwkAJAkALQkANgkAPwkAABIACRIAEhIAGxIAJBIALRIANhIAPxIAABsACRsAEhsAGxsAJBsALRsANhsAPxsAACQACSQAEiQAGyQAJCQALSQANiQAPyQAAC0ACS0AEi0AGy0AJC0ALS0ANi0APy0AADYACTYAEjYAGzYAJDYALTYANjYAPzYAAD8ACT8AEj8AGz8AJD8ALT8ANj8APz8AAAAVSQAVUgAVWwAVZAAVbQAVdgAVfwAVQAkVSQkVUgkVWwkVZAkVbQkVdgkVfwkVQBIVSRIVUhIVWxIVZBIVbRIVdhIVfxIVQBsVSRsVUhsVWxsVZBsVbRsVdhsVfxsVQCQVSSQVUiQVWyQVZCQVbSQVdiQVfyQVQC0VSS0VUi0VWy0VZC0VbS0Vdi0Vfy0VQDYVSTYVUjYVWzYVZDYVbTYVdjYVfzYVQD8VST8VUj8VWz8VZD8VbT8Vdj8Vfz8VQAAqiQAqkgAqmwAqpAAqrQAqtgAqvwAqgAkqiQkqkgkqmwkqpAkqrQkqtgkqvwkqgBIqiRIqkhIqmxIqpBIqrRIqthIqvxIqgBsqiRsqkhsqmxsqpBsqrRsqthsqvxsqgCQqiSQqkiQqmyQqpCQqrSQqtiQqvyQqgC0qiS0qki0qmy0qpC0qrS0qti0qvy0qgDYqiTYqkjYqmzYqpDYqrTYqtjYqvzYqgD8qiT8qkj8qmz8qpD8qrT8qtj8qvz8qgAA/yQA/0gA/2wA/5AA/7QA/9gA//wA/wAk/yQk/0gk/2wk/5Ak/7Qk/9gk//wk/wBI/yRI/0hI/2xI/5BI/7RI/9hI//xI/wBs/yRs/0hs/2xs/5Bs/7Rs/9hs//xs/wCQ/ySQ/0iQ/2yQ/5CQ/7SQ/9iQ//yQ/wC0/yS0/0i0/2y0/5C0/7S0/9i0//y0/wDY/yTY/0jY/2zY/5DY/7TY/9jY//zY/wD8/yT8/0j8/2z8/5D8/7T8/9j8//z8/yH/C05FVFNDQVBFMi4wAwEAAAAh+QQEZAAfACwAAAAAEAAMAAAIlgAPCDyAA8ePH4CAKTx2DB++gQQN/lC4sOHDAwIEQIAgIQIAIB8FCIEAxcCBAAIQQEggAYhLIEGEIIEiMCMEBBIkfAwZhKRAlCpZvoQpYGZNARESmDCzNABKIUt/pmTjgYKAM6RQmjLD4OiJDzk/nhj7wedJAR9OCAWQNq1RjAK+LjBjJsDXrwrKmESZdoGJLmjvKmgTEAA7",
        ),
        fixture(
            name = "project-gradient.webp",
            mime = "image/webp",
            kind = AImageInputKind.WEBP,
            sha256 = "1068a89a88d4926f1b63803f35d5d4425f05153c048ddeec23f899a2c277efab",
            base64 = "UklGRtoAAABXRUJQVlA4IM4AAABQAwCdASoQAAwAAgA0JbACdDXAP1A6xr9YPSAKYMQH/0fLgAD++KVWNB9XJXX5exNfcz83pf7Y1mhQVd3QNczvJXYY4ZofuFdy06Y+Lb4WJpLR9bH/VlG187HEry4V4KUBLfjIqwz7+Uf0jHy51u5r7/7eux3k3f8x4Lz2gPqKzej/8iiH/5oI1t8n/H8jvfrKj2Jtf/f5bCvxf/Y1LJ/ltMgc9/8TH/rnV7mFwz/3rDjhGIh+/k4n6Rjc4v/6WHQZnkgO33TfZf5xv6tAAA==",
        ),
        fixture(
            name = "project-gradient.bmp",
            mime = "image/bmp",
            kind = AImageInputKind.BMP,
            sha256 = "5b96666cbdc817fdab5c4a774d4300462001696d934c6a8e8d825a7f1c7a32e1",
            base64 = "Qk12AgAAAAAAADYAAAAoAAAAEAAAAAwAAAABABgAAAAAAEACAAAAAAAAAAAAAAAAAAAAAAAAAAD9AAD9AAA+AAA+AH/+AH/+AC5uLo7OaIfGAAA1AH/+AH/+AH/+AH/+AClIVJSzAAD9AAD9AAA+AAA+AH/+AH/+AC5uLo7OaIfGAAA1AH/+AH/+AH/+AH/+AClIVJSzAAD9AAD9AAA+AAA+AH/+AH/+AD09AD09PgAAPgAAAH/+AH/+AH/+AH/+Pj4APj4AAAD9AAD9AAA+AAA+AH/+AH/+AD09AD09PgAAPgAAAH/+AH/+AH/+AH/+Pj4APj4AAAD9AAD9AAA+AAA+ADEjVqOVF4bmACaGAABSNILjnYqrDAAaDgBPjHzNKonaAB5vAAD9AAD9AAA+AAA+ADEjADEjF4bmF4bmNILjNILjDAAaDAAaDgBPDgBPKonaKonaAAD9AAD9AAA+AAA+AD4AAD4AAD09AD09PgAAPgAAPgAAPgAAPQA9PQA9Pj4APj4AAAD9AAD9AAA+AAA+AD4AAD4AAD09AD09PgAAPgAAPgAAPgAAPQA9PQA9Pj4APj4AAAD9AAD9AAA+AAA+AD4AAD4AAD09AD09PgAAPgAAPgAAPgAAPQA9PQA9Pj4APj4AAAD9AAD9AAA+AAA+AD4AAD4AAD09AD09PgAAPgAAPgAAPgAAPQA9PQA9Pj4APj4AAAD9AAD9AAD9AAD9AP4AAP4AAP39AP39/gAA/gAA/gAA/gAA/QD+/QD+//8A//8AAAD9AAD9AAD9AAD9AP4AAP4AAP39AP39/gAA/gAA/gAA/gAA/QD+/QD+//8A//8A",
        ),
        fixture(
            name = "project-gradient.ico",
            mime = "image/x-ico",
            kind = AImageInputKind.ICO,
            sha256 = "7c213793e4f3222ac6a91a43702ec97d1b6aae113d8582df3ac7cf090da0ff91",
            base64 = "AAABAAEAEAwAAAEAIAAJAQAAFgAAAIlQTkcNChoKAAAADUlIRFIAAAAQAAAADAgCAAAA5IWq1gAAAAlwSFlzAAAAAQAAAAEATyXE1gAAALtJREFUeJxj/MuAAMz/QOTf/2A2hAMG/xhAQkz/mRgYGFgYSAQsEMoRQjmAif9IHDCwB9vAYM9IgQ300qD8GUSuDBUHhZXrNwYGBvO6GwwMDEIMoPC55S2DxYaiojCGxwcXLHh55tbLiU0mE7sm5pflMzAw1NXUMTDMmzr1GoqG234MDHdW2e9jYBBnOHzLbl0dw7/65OB6iGQJ01Y7BgU7anja8CNDQoLeDy+t/MoZyOIerw9lZ4eQbAMAf1c2QFq1QT8AAAAASUVORK5CYII=",
        ),
        fixture(
            name = "project-checker.wbmp",
            mime = "image/vnd.wap.wbmp",
            kind = AImageInputKind.WBMP,
            sha256 = "127226e46f2659a21dcb959b61c01aa5c27eb11294e03694f77b4f2e21ab4deb",
            base64 = "AAAQDPDw8PDw8PDwDw8PDw8PDw/w8PDw8PDw8A==",
        ),
    )

    /**
     * These are intentionally not vendored. Fetch them from the immutable URLs,
     * verify SHA-256, then place them in the device corpus directory documented
     * by the qualification guide. Missing files make a run preliminary only.
     */
    val external: List<ExternalFixtureSpec> = listOf(
        external(
            "translucent-green-p3.png", "image/png", AImageInputKind.PNG,
            "9f1bd663564634bff9d7f3c25a9495ac71c8565a6a9407a64acfae2bc33e1c57",
        ),
        external(
            "blue-16bit-srgb.png", "image/png", AImageInputKind.PNG,
            "aa39b12b96bba7084648902af956f6563f361d8631400396344807ce919cb6db",
        ),
        external(
            "red-hlg-profile.png", "image/png", AImageInputKind.PNG,
            "9cf5df965aefb69ac6dc9845055c8a84309879dc1f451074cb632159cbb4a193",
        ),
        external(
            "red-pq-profile.png", "image/png", AImageInputKind.PNG,
            "a2b2a147067b0e019ed7768abc424dc7755694fe920021517d3be8257338cb6b",
        ),
        ExternalFixtureSpec(
            fileName = "animated.gif",
            mime = "image/gif",
            kind = AImageInputKind.GIF,
            expectedSha256 = "eec5e745032b9775d67f040d9ab95ae3dc296100ce0c5d6bf95667bf2d27d2a6",
            upstreamUrl = "https://android.googlesource.com/platform/cts/+/1daba777fa1cc472226da4104041849ccbc65b80/tests/tests/graphics/res/drawable/animated.gif?format=TEXT",
        ),
        ExternalFixtureSpec(
            fileName = "heifwriter_input.heic",
            mime = "image/heic",
            kind = AImageInputKind.HEIF,
            expectedSha256 = "62dfb44160403ca8355a874cecc91cdbce57e98dd597fa36a2af55ef54c017ac",
            upstreamUrl = "https://android.googlesource.com/platform/cts/+/41ac7864bded41f9c042bd9e3cf9a2c083f23da9/tests/tests/media/res/raw/heifwriter_input.heic?format=TEXT",
        ),
        ExternalFixtureSpec(
            fileName = "sample_1mp.dng",
            mime = "image/x-adobe-dng",
            kind = AImageInputKind.DNG,
            expectedSha256 = "271aa1db6369f271e160acaf3029c8e86b8a86d2e9a44d1cc731f50575767ac0",
            displayReferredDngFallback = true,
            upstreamUrl = "https://android.googlesource.com/platform/cts/+/1daba777fa1cc472226da4104041849ccbc65b80/tests/tests/graphics/res/raw/sample_1mp.dng?format=TEXT",
        ),
        ExternalFixtureSpec(
            fileName = "bug_156261521.dng",
            mime = "image/x-adobe-dng",
            kind = AImageInputKind.DNG,
            expectedSha256 = "8b0237910cc4ff180ad96fb1af42ef4a5b1edd92f9fa2cb274638a97b20db544",
            displayReferredDngFallback = true,
            hostileOnly = true,
            upstreamUrl = "https://android.googlesource.com/platform/cts/+/1daba777fa1cc472226da4104041849ccbc65b80/tests/tests/security/res/raw/bug_156261521.dng?format=TEXT",
        ),
    )

    private fun fixture(
        name: String,
        mime: String,
        kind: AImageInputKind,
        sha256: String,
        base64: String,
        orientation: Int = 1,
    ) = QualificationFixture(
        name = name,
        mime = mime,
        kind = kind,
        expectedSha256 = sha256,
        encodedOrientation = orientation,
        provenance = "Spektrafilm project-authored deterministic fixture",
        bytes = Base64.decode(base64, Base64.DEFAULT),
    )

    private fun external(
        fileName: String,
        mime: String,
        kind: AImageInputKind,
        sha256: String,
    ) = ExternalFixtureSpec(
        fileName = fileName,
        mime = mime,
        kind = kind,
        expectedSha256 = sha256,
        upstreamUrl = "https://android.googlesource.com/platform/cts/+/1daba777fa1cc472226da4104041849ccbc65b80/tests/tests/graphics/assets/$fileName?format=TEXT",
    )
}

internal fun sha256(bytes: ByteArray): String =
    MessageDigest.getInstance("SHA-256").digest(bytes).toHex()

internal fun ByteArray.toHex(): String = joinToString(separator = "") { byte ->
    "%02x".format(byte.toInt() and 0xff)
}
