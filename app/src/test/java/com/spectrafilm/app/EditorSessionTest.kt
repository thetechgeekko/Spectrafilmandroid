/*
 * Spektrafilm for Android — editor-session process-recreation regressions. GPLv3.
 */
package com.spectrafilm.app

import com.spectrafilm.app.masks.BlendMode
import com.spectrafilm.app.masks.LocalAdjustment
import com.spectrafilm.app.masks.LuminanceRange
import com.spectrafilm.app.masks.Mask
import com.spectrafilm.app.masks.MaskComponent
import com.spectrafilm.app.masks.MaskJson
import com.spectrafilm.app.masks.TierADelta
import java.io.IOException
import java.math.BigDecimal
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.runBlocking
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class EditorSessionTest {
    @get:Rule
    val temporary = TemporaryFolder()

    @Test
    fun ticketProbeIsDefaultOffAndHasNoEditorDestinationSeed() {
        Ticket139EditorTestBridge.reset()
        assertFalse(Ticket139EditorProbe.isArmed())
        Ticket139EditorProbe.publishCheckpoint()
        assertThrows(IllegalStateException::class.java) {
            Ticket139EditorTestBridge.checkpointGeneration()
        }

        try {
            Ticket139EditorTestBridge.arm()
            assertTrue(Ticket139EditorProbe.isArmed())
            assertEquals("", Ticket139EditorTestBridge.currentDestination())
            Ticket139EditorTestBridge.requestDestination(Screen.SETTINGS.name)
            assertEquals("", Ticket139EditorTestBridge.currentDestination())
        } finally {
            Ticket139EditorTestBridge.reset()
        }
        assertFalse(Ticket139EditorProbe.isArmed())
    }

    @Test
    fun activityOracleIsCanonicalAndEverySnapshotExercisesEveryPersistedParamsFamily() {
        val fixture = Ticket139EditorProbe.activityFixtureOracle()
        assertEquals(
            fixture,
            EditorSessionDocumentCodec.decode(EditorSessionDocumentCodec.encode(fixture)),
        )
        assertEquals(fixture.current, fixture.committed)
        val paramsSnapshots = buildList {
            add(fixture.current.paramsJson)
            fixture.committed?.paramsJson?.let(::add)
            fixture.history.undo.mapTo(this) { it.paramsJson }
            fixture.history.redo.mapTo(this) { it.paramsJson }
            fixture.preset.baseJson?.let(::add)
            fixture.preset.fullJson?.let(::add)
            fixture.preset.clipboardJson?.let(::add)
        }

        assertTrue(paramsSnapshots.size >= 7)
        val expectedExposures = listOf("1.25", "1.25", "-0.5", "2", "-0.5", "2", "1.25")
        paramsSnapshots.forEachIndexed { index, encoded ->
            runCatching {
                Ticket139EditorProbe.verifyPersistedParamsSentinels(
                    encoded,
                    expectedExposures[index],
                )
            }
                .getOrElse { throw AssertionError("snapshot $index lacks field-complete sentinels", it) }
        }
        val current = ParamsState().also {
            Presets.decode(AtomicJsonStore.parseObject(fixture.current.paramsJson), it)
        }
        // The native ingress currently accepts linear ProPhoto only. Other persisted input
        // sentinels still make the family non-default, but an unsupported colour-space label
        // prevents the preview bitmap from existing and therefore prevents a restored overlay
        // from ever entering composition on the physical release path.
        assertEquals("ProPhoto RGB", current.inputColorSpace)
        assertTrue(current.crop)
        assertEquals(0.43f to 0.57f, current.cropCenter)
        assertEquals(0.62f to 0.38f, current.cropSize)
        assertEquals(
            "rich fixture must survive the Params codec without changing the cursor",
            fixture.current.paramsJson,
            Presets.encode(current).toString(),
        )
    }

    @Test
    fun reconciledSessionIsTheOnlyRestorationAuthority() {
        val sourceA = EditorSourceState(
            uri = "content://photos/authoritative/a",
            kind = SourceKind.PHOTO,
            displayName = "a.heic",
            authorizationRequired = false,
        )
        val documentA = validDemoDocument().copy(source = sourceA)
        val durableA = PersistedSourceRef(
            uri = requireNotNull(sourceA.uri),
            kind = sourceA.kind.name,
            displayName = sourceA.displayName,
            accessMode = SourceAccessMode.PERSISTED,
        )
        val staleFallbackB = EditorSavedFallback(
            source = sourceA.copy(uri = "content://photos/stale/b", displayName = "b.heic"),
            rotationDegrees = 270,
        )

        val restored = reconcileEditorRestoration(
            session = documentA,
            sourceRestore = SourceRestoreResult.Ready(durableA),
            savedFallback = staleFallbackB,
        )

        assertTrue(restored.retainedSessionCursor)
        assertFalse(restored.usedSavedFallback)
        assertEquals(documentA.current, restored.document?.current)
        assertEquals(sourceA.uri, restored.source.uri)
        assertEquals(documentA.current.rotationDegrees, restored.rotationDegrees)
    }

    @Test
    fun changedDurableSourceDiscardsOldCursorBeforeEditorComposition() {
        val sourceA = EditorSourceState(
            uri = "content://photos/old/a",
            kind = SourceKind.PHOTO,
            displayName = "a.heic",
            authorizationRequired = false,
        )
        val documentA = validDemoDocument().copy(
            source = sourceA,
            current = EditSnapshot(paramsJson(exposure = 2f), 180),
        )
        val durableB = PersistedSourceRef(
            uri = "content://photos/new/b",
            kind = SourceKind.RAW.name,
            displayName = "b.dng",
            accessMode = SourceAccessMode.PERSISTED,
        )

        val restored = reconcileEditorRestoration(
            session = documentA,
            sourceRestore = SourceRestoreResult.Ready(durableB),
            savedFallback = EditorSavedFallback(sourceA, 180),
        )

        assertNull(restored.document)
        assertFalse(restored.retainedSessionCursor)
        assertEquals(durableB.uri, restored.source.uri)
        assertEquals(SourceKind.RAW, restored.source.kind)
        assertEquals(0, restored.rotationDegrees)
    }

    @Test
    fun revokedOrTransientSessionNeverSilentlyFallsBackToDemo() {
        val source = EditorSourceState(
            uri = "content://photos/revoked/session",
            kind = SourceKind.RAW,
            displayName = "session.dng",
            authorizationRequired = false,
        )
        val session = validDemoDocument().copy(source = source)

        val restored = reconcileEditorRestoration(
            session = session,
            sourceRestore = SourceRestoreResult.None,
            savedFallback = EditorSavedFallback.Empty,
        )

        assertEquals(source.uri, restored.source.uri)
        assertEquals(SourceKind.RAW, restored.source.kind)
        assertTrue(restored.source.authorizationRequired)
        assertEquals(session.current, restored.document?.current)
    }

    @Test
    fun roundTripPreservesSourceCursorHistoryMaskToolClipboardPresetAndPendingExport() {
        val base = paramsJson(exposure = 0f, withMask = true)
        val edited = paramsJson(exposure = 1.25f, withMask = true)
        val forward = paramsJson(exposure = 2f, withMask = true)
        val document = EditorSessionDocument(
            source = EditorSourceState(
                uri = "content://photos/library/42",
                kind = SourceKind.PHOTO,
                displayName = "portrait wide gamut.heic",
                authorizationRequired = false,
            ),
            current = EditSnapshot(edited, 90),
            committed = EditSnapshot(edited, 90),
            history = EditHistoryState(
                undo = listOf(EditSnapshot(base, 0)),
                redo = listOf(EditSnapshot(forward, 180)),
            ),
            tool = EditorToolState(Category.MASKS, EditorOverlayTool.MASK_SAMPLE_LUMINANCE, 0),
            preset = EditorPresetState(
                baseJson = base,
                fullJson = forward,
                amount = 0.35f,
                clipboardJson = edited,
                selectedPreset = "Night portrait",
                presetName = "My look",
            ),
            export = EditorExportState(
                sheetOpen = false,
                options = ExportOptions(ExportFormat.TIFF32F, 97, ExportSize.FULL, 2048, "final"),
                keepGps = false,
                phase = EditorExportPhase.RUNNING,
                runtimeRunId = 77L,
            ),
        )

        val encoded = EditorSessionDocumentCodec.encode(document)
        val restored = EditorSessionDocumentCodec.decode(encoded)

        assertEquals(document.source, restored.source)
        assertEquals(90, restored.current.rotationDegrees)
        assertEquals(1, restored.history.undo.size)
        assertEquals(1, restored.history.redo.size)
        assertEquals(document.tool, restored.tool)
        assertEquals(0.35f, restored.preset.amount, 0f)
        assertEquals("Night portrait", restored.preset.selectedPreset)
        assertEquals(EditorExportPhase.RUNNING, restored.export.phase)
        assertEquals(77L, restored.export.runtimeRunId)

        val params = ParamsState()
        Presets.decode(JSONObject(restored.current.paramsJson), params)
        assertEquals(1.25f, params.exposureCompensationEv, 0f)
        assertEquals(1, params.localAdjustments.size)
        assertEquals(0.75f, params.localAdjustments.single().delta.exposureEv, 0f)

        val clipboard = ParamsState()
        Presets.decode(JSONObject(requireNotNull(restored.preset.clipboardJson)), clipboard)
        assertEquals(1.25f, clipboard.exposureCompensationEv, 0f)
    }

    @Test
    fun futureVersionIsPreservedByReturningUnsupportedInsteadOfCoercing() {
        val root = JSONObject(EditorSessionDocumentCodec.encode(validDemoDocument()))
            .put("version", EditorSessionDocumentCodec.SCHEMA_VERSION + 1)

        assertThrows(UnsupportedEditorSessionException::class.java) {
            EditorSessionDocumentCodec.decode(root.toString())
        }
    }

    @Test
    fun fractionalVersionAndCorruptToolFailClosed() {
        val fractional = JSONObject(EditorSessionDocumentCodec.encode(validDemoDocument()))
            .put("version", BigDecimal("1.0000000000000001"))
        val fractionalFailure = assertThrows(IllegalArgumentException::class.java) {
            EditorSessionDocumentCodec.decode(fractional.toString())
        }
        assertTrue(fractionalFailure.message.orEmpty().contains("version"))

        val corruptTool = JSONObject(EditorSessionDocumentCodec.encode(validDemoDocument()))
        corruptTool.getJSONObject("tool").put("overlay", "EXECUTE_NATIVE_POINTER")
        assertThrows(IllegalArgumentException::class.java) {
            EditorSessionDocumentCodec.decode(corruptTool.toString())
        }
    }

    @Test
    fun fractionalExactIntegerIsDeterministicCorruptionAndQuarantinedOnce() {
        val target = temporary.newFolder("session-fractional-version").resolve("current.json")
        val fractional = JSONObject(EditorSessionDocumentCodec.encode(validDemoDocument()))
            .put("version", BigDecimal("1.0000000000000001"))
            .toString()
        AtomicJsonStore.writeUtf8(
            target,
            fractional,
            EditorSessionDocumentCodec.MAX_DOCUMENT_BYTES,
        )

        val result = EditorSessionStore.read(target)

        assertTrue(result is EditorSessionReadResult.CorruptQuarantined)
        assertFalse(target.exists())
        val quarantined = requireNotNull(target.parentFile).listFiles().orEmpty().single {
            it.name.startsWith("${target.name}.corrupt-")
        }
        assertEquals(fractional, quarantined.readText())
        assertEquals(EditorSessionReadResult.Missing, EditorSessionStore.read(target))
    }

    @Test
    fun typedParamsOracleRejectsSwappedAndDuplicatedNonDefaultSentinels() {
        val original = AtomicJsonStore.parseObject(
            Ticket139EditorProbe.activityFixtureOracle().current.paramsJson,
        )
        val swapped = AtomicJsonStore.parseObject(original.toString())
        val swappedGrade = swapped.getJSONObject("grade")
        val saturation = swappedGrade.get("saturation")
        val vibrance = swappedGrade.get("vibrance")
        swappedGrade.put("saturation", vibrance)
        swappedGrade.put("vibrance", saturation)
        assertThrows(IllegalStateException::class.java) {
            Ticket139EditorProbe.verifyPersistedParamsSentinels(swapped.toString())
        }

        val duplicated = AtomicJsonStore.parseObject(original.toString())
        val duplicatedGrade = duplicated.getJSONObject("grade")
        duplicatedGrade.put("saturation", duplicatedGrade.get("contrast"))
        assertThrows(IllegalStateException::class.java) {
            Ticket139EditorProbe.verifyPersistedParamsSentinels(duplicated.toString())
        }
    }

    @Test
    fun missingSelectedMaskIsRejectedBeforeOverlayCanRestore() {
        val document = validDemoDocument().copy(
            tool = EditorToolState(Category.MASKS, EditorOverlayTool.MASK_GEOMETRY, 0),
        )

        assertThrows(IllegalArgumentException::class.java) {
            EditorSessionDocumentCodec.encode(document)
        }
    }

    @Test
    fun cropAndGeometrySelectionsRestoreAsCleanDraftsWithoutMutatingEditsOrHistory() {
        assertEquals(
            EditorOverlayTool.CROP,
            restorableEditorOverlay(EditorOverlayTool.CROP),
        )
        assertEquals(
            EditorOverlayTool.MASK_GEOMETRY,
            restorableEditorOverlay(EditorOverlayTool.MASK_GEOMETRY),
        )

        val params = paramsJsonWithMasks(3)
        val baseline = validDemoDocument().copy(
            current = EditSnapshot(params, 270),
            committed = EditSnapshot(params, 270),
            history = EditHistoryState(
                undo = listOf(EditSnapshot(paramsJson(-1f), 90)),
                redo = listOf(EditSnapshot(paramsJson(1f), 180)),
            ),
            tool = EditorToolState(Category.MASKS, EditorOverlayTool.CROP, 2),
        )
        val crop = EditorSessionDocumentCodec.decode(EditorSessionDocumentCodec.encode(baseline))
        val geometry = EditorSessionDocumentCodec.decode(
            EditorSessionDocumentCodec.encode(
                baseline.copy(
                    tool = baseline.tool.copy(overlay = EditorOverlayTool.MASK_GEOMETRY),
                ),
            ),
        )
        val neutral = EditorSessionDocumentCodec.decode(
            EditorSessionDocumentCodec.encode(
                baseline.copy(tool = baseline.tool.copy(overlay = EditorOverlayTool.NONE)),
            ),
        )

        assertEquals(EditorOverlayTool.CROP, crop.tool.overlay)
        assertEquals(EditorOverlayTool.MASK_GEOMETRY, geometry.tool.overlay)
        assertEquals(neutral.current, crop.current)
        assertEquals(neutral.current, geometry.current)
        assertEquals(neutral.committed, crop.committed)
        assertEquals(neutral.committed, geometry.committed)
        assertEquals(neutral.history, crop.history)
        assertEquals(neutral.history, geometry.history)
        assertEquals(crop.copy(tool = geometry.tool), geometry)
    }

    @Test
    fun revokedGrantForSameUriPreservesCursorAndRequestsAuthorization() {
        val saved = EditorSourceState(
            uri = "content://photos/revoked/7",
            kind = SourceKind.RAW,
            displayName = "frame.dng",
            authorizationRequired = false,
        )
        val same = PersistedSourceRef(
            uri = saved.uri!!,
            kind = saved.kind.name,
            displayName = saved.displayName,
            accessMode = SourceAccessMode.PERSISTED,
        )
        val replacement = same.copy(uri = "content://photos/other/8")

        assertTrue(
            shouldRetainEditorCursor(saved, SourceRestoreResult.NeedsAuthorization(same)),
        )
        assertFalse(
            shouldRetainEditorCursor(saved, SourceRestoreResult.Ready(replacement)),
        )
        assertTrue(
            "a transient picker has no durable grant after process death, but its edits remain",
            shouldRetainEditorCursor(saved, SourceRestoreResult.None),
        )
    }

    @Test
    fun everyCurrentEditorSubscreenRoundTripKeepsTheSameBoundedCursor() {
        val baseline = paramsJson(exposure = -0.5f)
        val current = paramsJson(exposure = 0.8f)
        val expected = validDemoDocument().copy(
            source = EditorSourceState(
                uri = "content://photos/navigation/19",
                kind = SourceKind.PHOTO,
                displayName = "navigation.heic",
                authorizationRequired = false,
            ),
            current = EditSnapshot(current, 270),
            committed = EditSnapshot(current, 270),
            history = EditHistoryState(
                undo = listOf(EditSnapshot(baseline, 90)),
                redo = emptyList(),
            ),
            tool = EditorToolState(Category.INPUT, EditorOverlayTool.NONE, 0),
        )
        var restored = expected

        listOf(
            Screen.SETTINGS,
            Screen.ABOUT,
            Screen.DIAGNOSTICS,
            Screen.CURVES_FILM,
            Screen.CURVES_PRINT,
        ).forEach { destination ->
            restored = EditorSessionDocumentCodec.decode(
                EditorSessionDocumentCodec.encode(restored),
            )
            assertEquals("source changed across $destination", expected.source, restored.source)
            assertEquals("rotation changed across $destination", 270, restored.current.rotationDegrees)
            assertEquals("category changed across $destination", Category.INPUT, restored.tool.category)
            assertEquals("undo changed across $destination", 1, restored.history.undo.size)
        }
    }

    @Test
    fun rapidSourceReplacementRejectsTheOldRenderAndResetsItsCursorPolicy() {
        val oldSource = EditorSourceState(
            uri = "content://photos/rapid/old",
            kind = SourceKind.PHOTO,
            displayName = "old.heic",
            authorizationRequired = false,
        )
        val replacement = PersistedSourceRef(
            uri = "content://photos/rapid/new",
            kind = SourceKind.PHOTO.name,
            displayName = "new.heic",
            accessMode = SourceAccessMode.PERSISTED,
        )
        val gate = RenderPublicationGate()
        val oldTicket = gate.begin(gate.nextRevision(), RenderPublicationPriority.SETTLE)

        assertFalse(
            shouldRetainEditorCursor(oldSource, SourceRestoreResult.Ready(replacement)),
        )
        gate.invalidate()
        assertFalse("the replaced source must never publish", gate.tryClaim(oldTicket))
        val newTicket = gate.begin(gate.nextRevision(), RenderPublicationPriority.DRAFT)
        assertTrue("the replacement source may publish", gate.tryClaim(newTicket))
    }

    @Test
    fun missingRuntimeTurnsRestoredRunningExportIntoReconciliationWithoutRetry() {
        val running = validDemoDocument().export.copy(
            phase = EditorExportPhase.RUNNING,
            runtimeRunId = 31L,
            sheetOpen = true,
        )

        val recreatedSameProcess = reconcileRestoredExport(running, activeRuntimeRunId = 31L)
        assertEquals(EditorExportPhase.RUNNING, recreatedSameProcess.phase)
        assertEquals(31L, recreatedSameProcess.runtimeRunId)

        val recreatedProcess = reconcileRestoredExport(running, activeRuntimeRunId = null)
        assertEquals(EditorExportPhase.RECONCILING, recreatedProcess.phase)
        assertNull(recreatedProcess.runtimeRunId)
        assertFalse(recreatedProcess.sheetOpen)
    }

    @Test
    fun exportRunOwnershipTracksRevokedSourcesButNeverAuthorizesTheirPublication() {
        // Ownership (durable cursor tracking) accepts the exact current binding even while the
        // source still needs authorization; publication must keep refusing it, and the legacy
        // UNBOUND ABI must have neither.
        val revoked = ExportSourceIdentityAuthority.bind(
            uri = "content://ticket139/revoked-raw",
            kind = SourceKind.RAW,
            authorizationRequired = true,
        )
        assertTrue(exportRunOwnedByEditor(revoked, revoked))
        assertFalse(exportPublicationAuthorized(revoked, revoked))
        assertFalse(exportRunOwnedByEditor(ExportSourceIdentity.UNBOUND, revoked))

        val replacement = ExportSourceIdentityAuthority.bind(
            uri = null,
            kind = SourceKind.DEMO,
            authorizationRequired = false,
        )
        // The stale generation lost ownership; the new authorization-free one has both.
        assertFalse(exportRunOwnedByEditor(revoked, revoked))
        assertTrue(exportRunOwnedByEditor(replacement, replacement))
        assertTrue(exportPublicationAuthorized(replacement, replacement))
    }

    @Test
    fun restoredCursorAdoptsAnAuthorizedAlreadyRunningExport() {
        // Ticket #139: an export the process runtime is still running for THIS editor's
        // publication-authorized source identity is the editor's own work. A recreated
        // Activity must resume showing it, not restart at the checkpointed IDLE.
        val idle = validDemoDocument().export

        val adopted = reconcileRestoredExport(idle, activeRuntimeRunId = 47L)
        assertEquals(EditorExportPhase.RUNNING, adopted.phase)
        assertEquals(47L, adopted.runtimeRunId)
        // Everything else about the pre-launch cursor survives adoption verbatim.
        assertEquals(idle.sheetOpen, adopted.sheetOpen)
        assertEquals(idle.options, adopted.options)
        assertEquals(idle.keepGps, adopted.keepGps)
    }

    @Test
    fun onlyRunningExportStateCanCarryAProcessRunId() {
        val impossible = validDemoDocument().copy(
            export = validDemoDocument().export.copy(
                phase = EditorExportPhase.SUCCESS,
                runtimeRunId = 41L,
            ),
        )

        assertThrows(IllegalArgumentException::class.java) {
            EditorSessionDocumentCodec.encode(impossible)
        }
    }

    @Test
    fun immutableCompleteCursorOracleRejectsMutationInEveryPersistedBranch() {
        val params = paramsJsonWithMasks(3)
        val current = EditSnapshot(params, 270)
        val oracle = validDemoDocument().copy(
            current = current,
            committed = current.copy(rotationDegrees = 180),
            history = EditHistoryState(
                undo = listOf(EditSnapshot(paramsJson(-1f), 90)),
                redo = listOf(EditSnapshot(paramsJson(2f), 180)),
            ),
            tool = EditorToolState(Category.MASKS, EditorOverlayTool.MASK_GEOMETRY, 2),
            preset = EditorPresetState(
                paramsJson(-1f),
                paramsJson(2f),
                0.4f,
                params,
                "selected",
                "saved",
            ),
            export = EditorExportState(
                sheetOpen = true,
                options = ExportOptions(ExportFormat.PNG16, 91, ExportSize.CUSTOM, 3072, "oracle"),
                keepGps = true,
                phase = EditorExportPhase.IDLE,
                runtimeRunId = null,
            ),
        )
        val success = oracle.copy(
            export = oracle.export.copy(phase = EditorExportPhase.SUCCESS),
        )
        verifyCompleteEditorCursor(success, oracle, EditorExportPhase.SUCCESS)
        verifyCompleteEditorCursor(
            oracle.copy(
                export = oracle.export.copy(
                    phase = EditorExportPhase.RUNNING,
                    runtimeRunId = 44L,
                ),
            ),
            oracle,
            EditorExportPhase.RUNNING,
            expectedRuntimeRunId = 44L,
        )

        val mutations = listOf(
            // Full source identity/grant state.
            success.copy(source = success.source.copy(uri = "content://oracle/changed")),
            success.copy(source = success.source.copy(kind = SourceKind.PHOTO)),
            success.copy(source = success.source.copy(displayName = "changed")),
            success.copy(source = success.source.copy(authorizationRequired = true)),
            // Current, committed, and every history snapshot include recipe and rotation.
            success.copy(current = success.current.copy(paramsJson = paramsJson(3f))),
            success.copy(current = success.current.copy(rotationDegrees = 90)),
            success.copy(committed = success.committed?.copy(paramsJson = paramsJson(3f))),
            success.copy(committed = success.committed?.copy(rotationDegrees = 90)),
            success.copy(committed = null),
            success.copy(
                history = success.history.copy(
                    undo = success.history.undo.map { it.copy(paramsJson = paramsJson(3f)) },
                ),
            ),
            success.copy(
                history = success.history.copy(
                    undo = success.history.undo.map { it.copy(rotationDegrees = 0) },
                ),
            ),
            success.copy(history = success.history.copy(undo = emptyList())),
            success.copy(
                history = success.history.copy(
                    redo = success.history.redo.map { it.copy(paramsJson = paramsJson(3f)) },
                ),
            ),
            success.copy(
                history = success.history.copy(
                    redo = success.history.redo.map { it.copy(rotationDegrees = 0) },
                ),
            ),
            success.copy(history = success.history.copy(redo = emptyList())),
            // Complete tool cursor, including durable open-overlay selection.
            success.copy(tool = success.tool.copy(category = Category.INPUT)),
            success.copy(tool = success.tool.copy(overlay = EditorOverlayTool.NONE)),
            success.copy(tool = success.tool.copy(maskIndex = 1)),
            // Preset amount anchors, amount/name/selection, and clipboard.
            success.copy(preset = success.preset.copy(baseJson = paramsJson(-2f))),
            success.copy(preset = success.preset.copy(fullJson = paramsJson(3f))),
            success.copy(preset = success.preset.copy(amount = 0.7f)),
            success.copy(preset = success.preset.copy(clipboardJson = null)),
            success.copy(preset = success.preset.copy(selectedPreset = "changed")),
            success.copy(preset = success.preset.copy(presetName = "changed")),
            // Complete export sheet, options, GPS, phase, and runtime contract.
            success.copy(export = success.export.copy(sheetOpen = false)),
            success.copy(
                export = success.export.copy(
                    options = success.export.options.copy(format = ExportFormat.JPEG),
                ),
            ),
            success.copy(
                export = success.export.copy(
                    options = success.export.options.copy(jpegQuality = 90),
                ),
            ),
            success.copy(
                export = success.export.copy(
                    options = success.export.options.copy(size = ExportSize.FULL),
                ),
            ),
            success.copy(
                export = success.export.copy(
                    options = success.export.options.copy(customLongEdge = 2048),
                ),
            ),
            success.copy(
                export = success.export.copy(
                    options = success.export.options.copy(customName = "changed"),
                ),
            ),
            success.copy(export = success.export.copy(keepGps = false)),
            success.copy(export = success.export.copy(phase = EditorExportPhase.IDLE)),
        )
        mutations.forEachIndexed { index, mutation ->
            assertThrows("oracle mutation $index", IllegalStateException::class.java) {
                verifyCompleteEditorCursor(mutation, oracle, EditorExportPhase.SUCCESS)
            }
        }
        assertThrows(IllegalStateException::class.java) {
            verifyCompleteEditorCursor(
                oracle.copy(
                    export = oracle.export.copy(
                        phase = EditorExportPhase.RUNNING,
                        runtimeRunId = 45L,
                    ),
                ),
                oracle,
                EditorExportPhase.RUNNING,
                expectedRuntimeRunId = 44L,
            )
        }
    }

    @Test
    fun nonzeroMaskSelectionSurvivesWithOverlayClosedAndIsClampedByPolicy() {
        val params = paramsJsonWithMasks(3)
        val document = validDemoDocument().copy(
            current = EditSnapshot(params, 270),
            committed = EditSnapshot(params, 270),
            tool = EditorToolState(Category.MASKS, EditorOverlayTool.NONE, 2),
        )

        val restored = EditorSessionDocumentCodec.decode(
            EditorSessionDocumentCodec.encode(document),
        )

        assertEquals(EditorOverlayTool.NONE, restored.tool.overlay)
        assertEquals(2, restored.tool.maskIndex)
        assertEquals(2, clampSelectedMaskIndex(restored.tool.maskIndex, 3))
        assertEquals(1, clampSelectedMaskIndex(restored.tool.maskIndex, 2))
        assertEquals(0, clampSelectedMaskIndex(restored.tool.maskIndex, 0))
    }

    @Test
    fun everyHistorySnapshotIsValidatedBeforeEncoding() {
        val invalidRotation = validDemoDocument().copy(
            history = EditHistoryState(
                undo = listOf(EditSnapshot(paramsJson(), 45)),
                redo = emptyList(),
            ),
        )
        val invalidParams = validDemoDocument().copy(
            history = EditHistoryState(
                undo = emptyList(),
                redo = listOf(EditSnapshot("[", 0)),
            ),
        )

        assertThrows(IllegalArgumentException::class.java) {
            EditorSessionDocumentCodec.encode(invalidRotation)
        }
        assertThrows(Exception::class.java) {
            EditorSessionDocumentCodec.encode(invalidParams)
        }
    }

    @Test
    fun canonicalSnapshotExpansionNearFourMiBRoundTripsAtTheDurableBoundary() {
        val noncanonical = expandingNonCanonicalParams(fieldCount = 173)
        val rawBytes = AtomicJsonStore.utf8Length(noncanonical)
        val canonical = AtomicJsonStore.parseObject(noncanonical).toString()
        val canonicalBytes = AtomicJsonStore.utf8Length(canonical)
        assertTrue("fixture must expand during JSONObject serialization", canonicalBytes > rawBytes)
        assertTrue(rawBytes < AtomicJsonStore.MAX_PRESET_BYTES)
        assertTrue(
            "fixture must exercise the inclusive four-MiB boundary",
            canonicalBytes in (AtomicJsonStore.MAX_PRESET_BYTES - 64 * 1024)..
                AtomicJsonStore.MAX_PRESET_BYTES,
        )
        val snapshot = EditSnapshot(noncanonical, 270)
        val document = validDemoDocument().copy(current = snapshot, committed = snapshot)

        val encoded = EditorSessionDocumentCodec.encode(document)
        val restored = EditorSessionDocumentCodec.decode(encoded)

        assertEquals(canonical, restored.current.paramsJson)
        assertEquals(canonical, restored.committed?.paramsJson)
        assertEquals(restored, EditorSessionDocumentCodec.decode(EditorSessionDocumentCodec.encode(restored)))
    }

    @Test
    fun canonicalSnapshotExpansionOverFourMiBIsRejectedAndQuarantinedOnRead() {
        val noncanonical = expandingNonCanonicalParams(fieldCount = 175)
        val rawBytes = AtomicJsonStore.utf8Length(noncanonical)
        val canonicalBytes = AtomicJsonStore.utf8Length(
            AtomicJsonStore.parseObject(noncanonical).toString(),
        )
        assertTrue("raw fixture must remain admissible", rawBytes < AtomicJsonStore.MAX_PRESET_BYTES)
        assertTrue(
            "canonical fixture must cross the per-snapshot cap",
            canonicalBytes > AtomicJsonStore.MAX_PRESET_BYTES,
        )
        val invalid = validDemoDocument().copy(
            current = EditSnapshot(noncanonical, 0),
            committed = null,
        )
        val encodeFailure = assertThrows(DocumentLimitException::class.java) {
            EditorSessionDocumentCodec.encode(invalid)
        }
        assertTrue(encodeFailure.message.orEmpty().contains("canonical JSON"))

        // Model an old/external writer that admitted raw bytes but never checked canonical bytes.
        val valid = validDemoDocument().copy(committed = null)
        val validEncoded = EditorSessionDocumentCodec.encode(valid)
        val validCanonicalParams = AtomicJsonStore.parseObject(valid.current.paramsJson).toString()
        val needle = "\"params\":$validCanonicalParams"
        val offset = validEncoded.indexOf(needle)
        assertTrue("current params marker missing from valid session", offset >= 0)
        val legacyText = validEncoded.replaceRange(
            offset,
            offset + needle.length,
            "\"params\":$noncanonical",
        )
        assertTrue(AtomicJsonStore.utf8Length(legacyText) < EditorSessionDocumentCodec.MAX_DOCUMENT_BYTES)
        assertThrows(DocumentLimitException::class.java) {
            EditorSessionDocumentCodec.decode(legacyText)
        }

        val target = temporary.newFolder("canonical-expansion").resolve("current.json")
        AtomicJsonStore.writeUtf8(
            target,
            legacyText,
            EditorSessionDocumentCodec.MAX_DOCUMENT_BYTES,
        )
        val result = EditorSessionStore.read(target)
        assertTrue(result is EditorSessionReadResult.CorruptQuarantined)
        assertFalse(target.exists())
        val quarantined = requireNotNull(target.parentFile).listFiles().orEmpty().single {
            it.name.startsWith("${target.name}.corrupt-")
        }
        assertEquals(AtomicJsonStore.utf8Length(legacyText).toLong(), quarantined.length())
    }

    @Test
    fun historyByteCapAccountsCanonicalRetainedSnapshots() {
        val noncanonical = expandingNonCanonicalParams(fieldCount = 125)
        val rawBytes = AtomicJsonStore.utf8Length(noncanonical).toLong()
        val canonicalBytes = AtomicJsonStore.utf8Length(
            AtomicJsonStore.parseObject(noncanonical).toString(),
        ).toLong()
        val entries = 3
        assertTrue((rawBytes + Int.SIZE_BYTES) * entries <= EditHistory.DEFAULT_BYTE_CAP)
        assertTrue((canonicalBytes + Int.SIZE_BYTES) * entries > EditHistory.DEFAULT_BYTE_CAP)
        val document = validDemoDocument().copy(
            history = EditHistoryState(
                undo = List(entries) { EditSnapshot(noncanonical, 0) },
                redo = emptyList(),
            ),
        )

        val failure = assertThrows(IllegalArgumentException::class.java) {
            EditorSessionDocumentCodec.encode(document)
        }
        assertTrue(failure.message.orEmpty().contains("history exceeds"))
    }

    @Test
    fun maximumMaskMultiHistoryDocumentHasSymmetricAggregateBoundaryAndRetention() {
        val dense = maximumMaskSnapshot()
        val base = validDemoDocument().copy(
            current = dense,
            committed = dense,
            tool = EditorToolState(Category.MASKS, EditorOverlayTool.NONE, 63),
            preset = EditorPresetState(
                baseJson = dense.paramsJson,
                fullJson = dense.paramsJson,
                amount = 0.5f,
                clipboardJson = dense.paramsJson,
                selectedPreset = "Dense mask look",
                presetName = "Dense mask look",
            ),
        )

        fun withHistory(count: Int): EditorSessionDocument = base.copy(
            history = EditHistoryState(
                undo = List(count) { index ->
                    val rotations = listOf(0, 90, 180, 270)
                    dense.copy(rotationDegrees = rotations[index % rotations.size])
                },
                redo = emptyList(),
            ),
        )

        var low = 0
        var high = EditHistory.DEFAULT_CAP
        while (low < high) {
            val candidate = (low + high + 1) / 2
            if (runCatching { EditorSessionDocumentCodec.encode(withHistory(candidate)) }.isSuccess) {
                low = candidate
            } else {
                high = candidate - 1
            }
        }
        val maximumFittingHistory = low
        assertTrue("fixture must exercise multiple history snapshots", maximumFittingHistory >= 2)
        assertTrue(
            "fixture must cross the aggregate root bound before the history entry cap",
            maximumFittingHistory < EditHistory.DEFAULT_CAP,
        )

        val fitting = withHistory(maximumFittingHistory)
        val restored = EditorSessionDocumentCodec.decode(EditorSessionDocumentCodec.encode(fitting))
        assertEquals(fitting.source, restored.source)
        assertEquals(fitting.current, restored.current)
        assertEquals(fitting.history, restored.history)
        assertEquals(fitting.preset, restored.preset)

        val overBudget = withHistory(maximumFittingHistory + 1)
        val failure = assertThrows(DocumentLimitException::class.java) {
            EditorSessionDocumentCodec.encode(overBudget)
        }
        assertTrue(failure.message.orEmpty().contains("node count"))

        var finalRootMaterializations = 0
        val retainedResult = retainEncodableEditorSession(overBudget) {
            finalRootMaterializations++
        }
        val retained = retainedResult.document
        assertEquals(overBudget.source, retained.source)
        assertEquals(overBudget.current, retained.current)
        assertEquals(overBudget.committed, retained.committed)
        assertEquals(
            overBudget.history.undo.takeLast(retained.history.undo.size),
            retained.history.undo,
        )
        assertEquals(overBudget.preset, retained.preset)
        assertEquals(
            "profiling must never materialize a candidate root",
            1,
            finalRootMaterializations,
        )
        assertEquals(
            retainedResult.encoded,
            EditorSessionDocumentCodec.encode(retained),
        )
        assertEquals(
            retained,
            EditorSessionDocumentCodec.decode(EditorSessionDocumentCodec.encode(retained)),
        )
    }

    @Test
    fun retentionDropsPresetAuxiliariesOnlyAfterAllHistoryAndNeverDropsCurrentOrSource() {
        val dense = maximumMaskSnapshot(withDenseExtension = true)
        val document = validDemoDocument().copy(
            current = dense,
            committed = dense,
            history = EditHistoryState(
                undo = listOf(dense.copy(rotationDegrees = 90)),
                redo = listOf(dense.copy(rotationDegrees = 180)),
            ),
            tool = EditorToolState(Category.MASKS, EditorOverlayTool.NONE, 63),
            preset = EditorPresetState(
                baseJson = dense.paramsJson,
                fullJson = dense.paramsJson,
                amount = 0.25f,
                clipboardJson = dense.paramsJson,
                selectedPreset = "Dense",
                presetName = "Dense",
            ),
        )

        assertThrows(DocumentLimitException::class.java) {
            EditorSessionDocumentCodec.encode(document)
        }
        var finalRootMaterializations = 0
        val retainedResult = retainEncodableEditorSession(document) {
            finalRootMaterializations++
        }
        val retained = retainedResult.document

        assertEquals(document.source, retained.source)
        assertEquals(document.current, retained.current)
        assertEquals(document.committed, retained.committed)
        assertTrue(retained.history.undo.isEmpty())
        assertTrue(retained.history.redo.isEmpty())
        assertNull(retained.preset.clipboardJson)
        assertNull(retained.preset.baseJson)
        assertNull(retained.preset.fullJson)
        assertEquals(1f, retained.preset.amount, 0f)
        assertEquals(1, finalRootMaterializations)
        assertEquals(
            retained,
            EditorSessionDocumentCodec.decode(EditorSessionDocumentCodec.encode(retained)),
        )
    }

    @Test
    fun costProfileMatchesFinalRootNodesDepthAndUtf8BytesExactly() {
        val extended = JSONObject(paramsJson(withMask = true)).apply {
            put("_escaped", "line\n\u2028\u20ac\uD83D\uDE42")
            put("_exactDecimal", BigDecimal("2.0000000000000001"))
            put("_nested", org.json.JSONArray().put(JSONObject().put("value", 7)))
        }.toString()
        val snapshot = EditSnapshot(extended, 270)
        val document = validDemoDocument().copy(
            current = snapshot,
            committed = snapshot,
            history = EditHistoryState(
                undo = listOf(snapshot.copy(rotationDegrees = 90)),
                redo = listOf(snapshot.copy(rotationDegrees = 180)),
            ),
            tool = EditorToolState(Category.MASKS, EditorOverlayTool.MASK_GEOMETRY, 0),
            preset = EditorPresetState(
                extended,
                extended,
                0.375f,
                extended,
                "sel\n\u20ac",
                "name\uD83D\uDE42",
            ),
            export = validDemoDocument().export.copy(
                sheetOpen = true,
                keepGps = true,
                options = ExportOptions(
                    ExportFormat.PNG16,
                    87,
                    ExportSize.CUSTOM,
                    4095,
                    "x\n\u20ac",
                ),
            ),
        )

        val profiled = EditorSessionDocumentCodec.measureEncodedCost(document)
        val encoded = EditorSessionDocumentCodec.encode(document)
        val actual = AtomicJsonStore.measureText(
            encoded,
            JsonStructureLimits(
                maxDepth = 48,
                maxNodes = 600_000,
                maxArrayLength = 128,
                maxObjectKeys = 4_096,
                maxStringChars = 16 * 1024,
                maxTokenChars = 128,
                maxInputChars = EditorSessionDocumentCodec.MAX_DOCUMENT_BYTES,
            ),
        )

        assertEquals(actual.nodeCount, profiled.nodeCount)
        assertEquals(actual.maxDepth, profiled.maxDepth)
        assertEquals(actual.utf8Bytes.toLong(), profiled.utf8Bytes)
        assertEquals(encoded.toByteArray(Charsets.UTF_8).size.toLong(), profiled.utf8Bytes)
    }

    @Test
    fun everyInvalidSubtreeFailsBeforeRetentionCanDropItOrMaterializeFinalRoot() {
        fun extended(value: Any): String = JSONObject(paramsJson())
            .put("_adversarial", value)
            .toString()

        val tooWideArray = extended(
            org.json.JSONArray().apply { repeat(129) { put(it) } },
        )
        val tooWideObject = extended(
            JSONObject().apply { repeat(4_097) { put("k$it", it) } },
        )
        val tooLongString = extended("x".repeat(16 * 1024 + 1))
        val tooLongToken = paramsJson().dropLast(1) + ",\"_adversarial\":" + "1".repeat(129) + "}"
        val tooDeep = extended(JSONObject("{\"v\":" + "[".repeat(41) + "0" + "]".repeat(41) + "}"))
        val dense = maximumMaskSnapshot(withDenseExtension = true)

        listOf(tooWideArray, tooWideObject, tooLongString, tooLongToken, tooDeep).forEachIndexed {
                index, invalid ->
            val document = validDemoDocument().copy(
                history = EditHistoryState(
                    undo = listOf(EditSnapshot(invalid, 0)),
                    redo = listOf(dense.copy(rotationDegrees = 180)),
                ),
                preset = EditorPresetState(
                    dense.paramsJson,
                    dense.paramsJson,
                    0.5f,
                    dense.paramsJson,
                    "dense",
                    "dense",
                ),
            )
            var finalRoots = 0

            assertThrows("invalid subtree $index", DocumentLimitException::class.java) {
                retainEncodableEditorSession(document) { finalRoots++ }
            }
            assertEquals("invalid subtree $index reached final materialization", 0, finalRoots)
        }
    }

    @Test
    fun removedProfileInAnyRestorableBranchInvalidatesHistoryAndAnchors() {
        val installed = ParamsState()
        val available = setOf(installed.filmProfile, installed.printProfile)
        val removed = ParamsState().also {
            Presets.decode(Presets.encode(installed), it)
            it.filmProfile = "removed/profile"
        }
        val removedJson = Presets.toJsonString(removed)
        val current = EditSnapshot(Presets.toJsonString(installed), 0)
        val base = validDemoDocument().copy(current = current, committed = current)

        val branches = listOf(
            base.copy(history = EditHistoryState(listOf(EditSnapshot(removedJson, 0)), emptyList())),
            base.copy(history = EditHistoryState(emptyList(), listOf(EditSnapshot(removedJson, 0)))),
            base.copy(preset = base.preset.copy(baseJson = removedJson)),
            base.copy(preset = base.preset.copy(fullJson = removedJson)),
            base.copy(preset = base.preset.copy(clipboardJson = removedJson)),
        )

        assertFalse(base.referencesUnavailableProfiles(available))
        branches.forEachIndexed { index, document ->
            assertTrue("branch $index must invalidate restoration anchors", document.referencesUnavailableProfiles(available))
        }
    }

    @Test
    fun checkpointOfferedAfterDrainBeforeReadIsIncludedInReturnedDocument() {
        val disk = AtomicReference("old")
        val readerEntered = CountDownLatch(1)
        val releaseReader = CountDownLatch(1)
        val reads = AtomicInteger(0)
        val coordinator = LatestCheckpointCoordinator<String>(disk::set)
        coordinator.offer("old")
        coordinator.flush()
        val executor = Executors.newSingleThreadExecutor()
        try {
            val restored = executor.submit<String> {
                coordinator.flushThenRead {
                    if (reads.incrementAndGet() == 1) {
                        readerEntered.countDown()
                        assertTrue(releaseReader.await(5, TimeUnit.SECONDS))
                    }
                    disk.get()
                }
            }
            assertTrue(readerEntered.await(5, TimeUnit.SECONDS))
            coordinator.offer("new")
            releaseReader.countDown()

            assertEquals("new", restored.get(5, TimeUnit.SECONDS))
            assertTrue("reader must retry after the concurrent checkpoint", reads.get() >= 2)
        } finally {
            releaseReader.countDown()
            executor.shutdownNow()
        }
    }

    @Test
    fun corruptClassificationAndQuarantineBlockAValidReplacementOnTheSamePathLock() {
        val target = temporary.newFolder("session-classification-race").resolve("current.json")
        AtomicJsonStore.writeUtf8(target, "{not-valid-json", 1024)
        val corruptBytesObserved = CountDownLatch(1)
        val releaseDecode = CountDownLatch(1)
        val writerAttempted = CountDownLatch(1)
        val writerCompleted = CountDownLatch(1)
        val result = AtomicReference<EditorSessionReadResult?>()
        val workerFailure = AtomicReference<Throwable?>()
        val replacement = EditorSessionDocumentCodec.encode(validDemoDocument())

        val classifier = Thread {
            try {
                result.set(
                    EditorSessionStore.read(
                        file = target,
                        readText = { file ->
                            AtomicJsonStore.readUtf8(file, 1024).also {
                                corruptBytesObserved.countDown()
                                assertTrue(releaseDecode.await(5, TimeUnit.SECONDS))
                            }
                        },
                    ),
                )
            } catch (failure: Throwable) {
                workerFailure.compareAndSet(null, failure)
            }
        }
        val writer = Thread {
            try {
                assertTrue(corruptBytesObserved.await(5, TimeUnit.SECONDS))
                writerAttempted.countDown()
                AtomicJsonStore.writeUtf8(
                    target,
                    replacement,
                    EditorSessionDocumentCodec.MAX_DOCUMENT_BYTES,
                )
                writerCompleted.countDown()
            } catch (failure: Throwable) {
                workerFailure.compareAndSet(null, failure)
            }
        }

        classifier.start()
        writer.start()
        assertTrue(writerAttempted.await(5, TimeUnit.SECONDS))
        assertFalse("replacement overtook decode/classification", writerCompleted.await(250, TimeUnit.MILLISECONDS))
        releaseDecode.countDown()
        classifier.join(5_000)
        writer.join(5_000)

        workerFailure.get()?.let { throw AssertionError("worker failed", it) }
        assertTrue(result.get() is EditorSessionReadResult.CorruptQuarantined)
        assertTrue(writerCompleted.count == 0L)
        assertEquals(
            EditorSessionDocumentCodec.decode(replacement),
            (EditorSessionStore.read(target) as EditorSessionReadResult.Loaded).document,
        )
        val quarantined = requireNotNull(target.parentFile).listFiles().orEmpty().single {
            it.name.startsWith("${target.name}.corrupt-")
        }
        assertEquals("{not-valid-json", quarantined.readText())
    }

    @Test
    fun transientReadIoFailureIsUnavailableAndNeverQuarantinesLastGoodGeneration() {
        val target = temporary.newFolder("session-transient-io").resolve("current.json")
        val encoded = EditorSessionDocumentCodec.encode(validDemoDocument())
        AtomicJsonStore.writeUtf8(target, encoded, EditorSessionDocumentCodec.MAX_DOCUMENT_BYTES)
        var quarantineCalled = false

        val result = EditorSessionStore.read(
            file = target,
            readText = { throw IOException("transient device error") },
            quarantine = { quarantineCalled = true },
        )

        assertTrue(result is EditorSessionReadResult.Unavailable)
        assertFalse(quarantineCalled)
        assertTrue(target.isFile)
        assertEquals(encoded, AtomicJsonStore.readUtf8(target, EditorSessionDocumentCodec.MAX_DOCUMENT_BYTES))
    }

    @Test
    fun backupOnlyInterruptedAtomicGenerationIsRecoveredBeforeMissingClassification() {
        val target = temporary.newFolder("session-backup-only").resolve("current.json")
        val backup = target.resolveSibling(target.name + ".bak")
        val expected = validDemoDocument()
        val encoded = EditorSessionDocumentCodec.encode(expected)
        val canonicalExpected = EditorSessionDocumentCodec.decode(encoded)
        backup.writeText(encoded, Charsets.UTF_8)
        assertFalse(target.exists())
        assertTrue(backup.isFile)

        val result = EditorSessionStore.read(target)

        assertTrue(result is EditorSessionReadResult.Loaded)
        assertEquals(canonicalExpected, (result as EditorSessionReadResult.Loaded).document)
        assertTrue("AtomicFile must restore the backup as the active base", target.isFile)
        assertFalse("the recovered backup generation must be consumed", backup.exists())
    }

    @Test
    fun failOnceCheckpointRemainsPendingAndFlushThenReadCannotReturnStaleSuccess() {
        val disk = AtomicReference("old")
        val attempts = AtomicInteger(0)
        val coordinator = LatestCheckpointCoordinator<String>(writer = { value ->
            if (attempts.incrementAndGet() == 1) throw IOException("fail once")
            disk.set(value)
        })
        coordinator.offer("new")

        assertThrows(IOException::class.java) {
            coordinator.flushThenRead { disk.get() }
        }
        assertEquals("old", disk.get())
        assertEquals("new", coordinator.flushThenRead { disk.get() })
        assertEquals(2, attempts.get())
    }

    @Test
    fun transientUnavailableCanRecoverAndTerminalOwnershipWaitsForDurableCheckpoint() = runBlocking {
        assertFalse(
            editorSessionWritesProtected(
                EditorSessionReadResult.Unavailable("injected transient read failure"),
            ),
        )
        assertEquals(
            EditorSessionWriteAccess.RECOVERING,
            editorSessionWriteAccess(
                EditorSessionReadResult.Unavailable("injected transient read failure"),
            ),
        )
        assertTrue(
            editorSessionWritesProtected(
                EditorSessionReadResult.Unsupported("future version"),
            ),
        )

        val recoveryReads = AtomicInteger(0)
        val recoveryWaits = AtomicInteger(0)
        val recovered = awaitEditorSessionWriteRecovery(
            read = {
                if (recoveryReads.incrementAndGet() == 1) {
                    EditorSessionReadResult.Unavailable("fail once")
                } else {
                    EditorSessionReadResult.Missing
                }
            },
            onUnavailable = { recoveryWaits.incrementAndGet() },
        )
        assertEquals(EditorSessionReadResult.Missing, recovered)
        assertEquals(2, recoveryReads.get())
        assertEquals(1, recoveryWaits.get())
        assertEquals(EditorSessionWriteAccess.WRITABLE, editorSessionWriteAccess(recovered))

        val checkpoints = AtomicInteger(0)
        val flushAttempts = AtomicInteger(0)
        val retries = AtomicInteger(0)
        val durable = awaitDurableEditorSessionCheckpoint(
            checkpoint = {
                checkpoints.incrementAndGet()
                true
            },
            flush = {
                if (flushAttempts.incrementAndGet() == 1) {
                    throw IOException("fail once")
                }
            },
            onRetryableFailure = { retries.incrementAndGet() },
        )
        assertTrue(durable)
        assertEquals(2, checkpoints.get())
        assertEquals(2, flushAttempts.get())
        assertEquals(1, retries.get())

        var protectedFlushes = 0
        val protected = awaitDurableEditorSessionCheckpoint(
            checkpoint = { false },
            flush = { protectedFlushes++ },
            onRetryableFailure = { throw AssertionError("protected write retried") },
        )
        assertFalse(protected)
        assertEquals(0, protectedFlushes)
    }

    @Test
    fun recoveredLoadedRichCursorWinsWhenLiveEditorWasNotMutated() {
        val recovered = validDemoDocument().copy(
            current = EditSnapshot(paramsJson(exposure = 1.25f, withMask = true), 90),
        )
        val live = validDemoDocument().copy(
            current = EditSnapshot(paramsJson(exposure = -0.5f), 180),
        )

        val resolved = resolveLoadedEditorSessionRecovery(
            recovered = recovered,
            sourceRestore = SourceRestoreResult.Demo,
            savedFallback = EditorSavedFallback.Empty,
            liveDocument = live,
            liveMutated = false,
        )

        assertEquals(EditorRecoveryConflictPolicy.RECOVERED_DURABLE, resolved.policy)
        assertEquals(recovered.current, resolved.restoration.document?.current)
        assertTrue(resolved.restoration.document?.current?.paramsJson?.contains("masks") == true)
    }

    @Test
    fun liveMutationWinsRecoveryOnlyAfterCurrentSourceReconciliation() {
        val recovered = validDemoDocument().copy(
            current = EditSnapshot(paramsJson(exposure = 1.25f), 90),
        )
        val live = validDemoDocument().copy(
            current = EditSnapshot(paramsJson(exposure = -0.75f, withMask = true), 270),
        )

        val resolved = resolveLoadedEditorSessionRecovery(
            recovered = recovered,
            sourceRestore = SourceRestoreResult.Demo,
            savedFallback = EditorSavedFallback.Empty,
            liveDocument = live,
            liveMutated = true,
        )

        assertEquals(EditorRecoveryConflictPolicy.LIVE_MUTATION, resolved.policy)
        assertEquals(live.current, resolved.restoration.document?.current)
    }

    @Test
    fun recipeAuthorityRequiresExactReadClassificationAndGeneration() {
        val key = "a".repeat(64)
        val pending: EditorRecipeAccess = EditorRecipeAccess.Pending(key, 7L)
        assertNull(pending.writableGenerationFor(key))
        assertTrue(
            classifyEditorRecipeAccess(key, 7L, 8L, RecipeReadResult.Missing) is
                EditorRecipeAccess.Pending,
        )
        assertTrue(
            classifyEditorRecipeAccess(
                key,
                8L,
                8L,
                RecipeReadResult.Unsupported("99"),
            ) is EditorRecipeAccess.Protected,
        )
        assertTrue(
            classifyEditorRecipeAccess(
                key,
                8L,
                8L,
                RecipeReadResult.IoFailure("transient"),
            ) is EditorRecipeAccess.Protected,
        )
        val writable = classifyEditorRecipeAccess(key, 8L, 8L, RecipeReadResult.Missing)
        assertEquals(8L, writable.writableGenerationFor(key))
        assertNull(writable.writableGenerationFor("b".repeat(64)))
    }

    @Test
    fun exportPublicationIsBoundToExactSourceAndAuthorizationGeneration() {
        val sourceA = ExportSourceIdentityAuthority.bind(
            "content://ticket139/source-a",
            SourceKind.PHOTO,
            authorizationRequired = false,
        )
        assertTrue(exportPublicationAuthorized(sourceA, sourceA))

        val sourceB = ExportSourceIdentityAuthority.bind(
            "content://ticket139/source-b",
            SourceKind.PHOTO,
            authorizationRequired = false,
        )
        assertFalse(exportPublicationAuthorized(sourceA, sourceB))
        assertFalse(exportPublicationAuthorized(sourceA, sourceA))
        assertTrue(exportPublicationAuthorized(sourceB, sourceB))

        val revokedB = ExportSourceIdentityAuthority.bind(
            "content://ticket139/source-b",
            SourceKind.PHOTO,
            authorizationRequired = true,
        )
        assertFalse(exportPublicationAuthorized(sourceB, revokedB))
        assertFalse(exportPublicationAuthorized(revokedB, revokedB))
    }

    @Test
    fun delayedOlderOfferCannotRegressAfterNewerOfferWasAlreadyDrained() {
        val firstAssigned = CountDownLatch(1)
        val releaseFirst = CountDownLatch(1)
        val disk = AtomicReference<String?>(null)
        val coordinator = LatestCheckpointCoordinator<String>(
            writer = disk::set,
            afterGenerationAssigned = { generation ->
                if (generation == 1L) {
                    firstAssigned.countDown()
                    assertTrue(releaseFirst.await(5, TimeUnit.SECONDS))
                }
            },
        )
        val executor = Executors.newSingleThreadExecutor()
        try {
            val older = executor.submit { coordinator.offer("old") }
            assertTrue(firstAssigned.await(5, TimeUnit.SECONDS))
            coordinator.offer("new")
            coordinator.flush()
            assertEquals("new", disk.get())
            releaseFirst.countDown()
            older.get(5, TimeUnit.SECONDS)
            coordinator.flush()

            assertEquals("new", disk.get())
        } finally {
            releaseFirst.countDown()
            executor.shutdownNow()
        }
    }

    @Test
    fun retiredActivityOwnerCannotOverwriteDirectReplacementFixture() {
        val disk = AtomicReference("old")
        val coordinator = LatestCheckpointCoordinator<String>(disk::set)
        val owners = EditorSessionCheckpointOwners()
        val outgoingActivity = owners.acquire()
        assertTrue(owners.offerIfCurrent(outgoingActivity) { coordinator.offer("outgoing") })

        // Probe/recovery replacement order: invalidate callbacks, drain every offer that crossed
        // the fence before invalidation, then install the direct replacement generation.
        owners.retire()
        coordinator.flush()
        disk.set("replacement")

        assertFalse(owners.offerIfCurrent(outgoingActivity) { coordinator.offer("stale-late") })
        coordinator.flush()
        assertEquals("replacement", disk.get())

        val replacementActivity = owners.acquire()
        assertTrue(owners.offerIfCurrent(replacementActivity) { coordinator.offer("new-live") })
        coordinator.flush()
        assertEquals("new-live", disk.get())
    }

    private fun validDemoDocument(): EditorSessionDocument {
        val current = EditSnapshot(paramsJson(), 0)
        return EditorSessionDocument(
            source = EditorSourceState(
                uri = null,
                kind = SourceKind.DEMO,
                displayName = "synthetic demo image",
                authorizationRequired = false,
            ),
            current = current,
            committed = current,
            history = EditHistoryState(emptyList(), emptyList()),
            tool = EditorToolState(null, EditorOverlayTool.NONE, 0),
            preset = EditorPresetState(null, null, 1f, null, "", ""),
            export = EditorExportState(
                sheetOpen = false,
                options = ExportOptions(ExportFormat.JPEG, 95, ExportSize.FULL, 2048, ""),
                keepGps = false,
                phase = EditorExportPhase.IDLE,
                runtimeRunId = null,
            ),
        )
    }

    private fun paramsJson(exposure: Float = 0f, withMask: Boolean = false): String {
        val state = ParamsState().apply {
            exposureCompensationEv = exposure
            if (withMask) {
                localAdjustments = listOf(
                    LocalAdjustment(
                        mask = Mask(
                            components = listOf(
                                Mask.Component(
                                    mode = BlendMode.ADD,
                                    shape = MaskComponent.Radial(
                                        cx = 0.4f,
                                        cy = 0.6f,
                                        rx = 0.25f,
                                        ry = 0.2f,
                                    ),
                                ),
                            ),
                            luminanceRange = LuminanceRange(lumMin = 0.35f, lumMax = 0.8f),
                        ),
                        delta = TierADelta(exposureEv = 0.75f),
                    ),
                )
            }
        }
        return Presets.toJsonString(state)
    }

    private fun paramsJsonWithMasks(count: Int): String {
        require(count >= 0)
        val state = ParamsState().apply {
            localAdjustments = List(count) { index ->
                LocalAdjustment(
                    mask = Mask(
                        components = listOf(
                            Mask.Component(
                                mode = BlendMode.ADD,
                                shape = MaskComponent.Radial(
                                    cx = 0.2f + index * 0.1f,
                                    cy = 0.6f,
                                    rx = 0.2f,
                                    ry = 0.2f,
                                ),
                            ),
                        ),
                    ),
                    delta = TierADelta(exposureEv = index.toFloat()),
                )
            }
        }
        return Presets.toJsonString(state)
    }

    /** `org.json` canonically escapes `/` after `<`, adding one byte per `</` pair. */
    private fun expandingNonCanonicalParams(fieldCount: Int): String {
        require(fieldCount > 0)
        val base = paramsJson()
        require(base.endsWith('}'))
        val expansion = "</".repeat(8_000)
        return buildString(base.length + fieldCount * (expansion.length + 32)) {
            append(base, 0, base.lastIndex)
            repeat(fieldCount) { index ->
                append(",\"_ticket139Expansion")
                append(index)
                append("\":\"")
                append(expansion)
                append('"')
            }
            append('}')
        }
    }

    private fun maximumMaskSnapshot(withDenseExtension: Boolean = false): EditSnapshot {
        val component = Mask.Component(
            mode = BlendMode.ADD,
            shape = MaskComponent.Radial(
                cx = 0.5f,
                cy = 0.5f,
                rx = 0.25f,
                ry = 0.25f,
            ),
        )
        val state = ParamsState().apply {
            localAdjustments = List(MaskJson.MAX_ADJUSTMENTS) {
                LocalAdjustment(
                    mask = Mask(
                        components = List(MaskJson.MAX_COMPONENTS_PER_MASK) { component },
                    ),
                    delta = TierADelta(exposureEv = 0.25f),
                )
            }
        }
        val root = JSONObject(Presets.toJsonString(state))
        if (withDenseExtension) {
            // Unknown fields are retained by the session snapshot but ignored by Presets. Arrays
            // stay inside the session's per-array bound while approaching its per-snapshot node cap.
            repeat(8) { extension ->
                root.put(
                    "_ticket139Dense$extension",
                    org.json.JSONArray().apply {
                        repeat(128) {
                            put(org.json.JSONArray().apply { repeat(128) { put(0) } })
                        }
                    },
                )
            }
        }
        return EditSnapshot(root.toString(), 0)
    }
}
