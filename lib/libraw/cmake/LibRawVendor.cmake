# Reproducible LibRaw source resolver shared by the Android build and host tests.
# The release archive is authoritative; the Git tag/commit are recorded for audit.
include_guard(GLOBAL)

set(SFRAW_PINNED_LIBRAW_VERSION "0.22.2")
set(SFRAW_PINNED_LIBRAW_URL
    "https://www.libraw.org/data/LibRaw-0.22.2.tar.gz")
set(SFRAW_PINNED_LIBRAW_SHA256
    "de86b035655accff8d4010f1a221fdf50d353cb7b1422ba26f14a0db92612cfa")
set(SFRAW_PINNED_LIBRAW_ARCHIVE_SIZE "1682962")
set(SFRAW_PINNED_LIBRAW_TAG_OBJECT
    "24fa7e5463cbf8b8615dbd2b16c933a294d52400")
set(SFRAW_PINNED_LIBRAW_COMMIT
    "b93f6e45c194f5df9b02a43b1af9a54b4f41f33f")

set(SFRAW_LIBRAW_SOURCE_DIR "" CACHE PATH
    "Debug/offline-only exact LibRaw 0.22.2 source; release builds use the hashed archive")

set(_SFRAW_LIBRAW_PATCH_DIR "${CMAKE_CURRENT_LIST_DIR}/../patches")
set(_SFRAW_LIBRAW_PATCHED_TREE_SHA256
    "bc463c30e414781d2455a47b99c30741830798b5941a049782b560fbb3abc74c")
# Exact aggregate shipped by the immediately preceding 24-patch resolver.
# It is accepted only as a migration source for the ICC conversion patch.
set(_SFRAW_LIBRAW_PATCHED_TREE_SHA256_24
    "bf5e58dc1c950f19110311b319a967f819930e656b0e8802ed4a4a133ae818d4")
# Exact aggregate shipped by the immediately preceding 23-patch resolver.
# It is accepted only as a migration source for the X-Trans arithmetic patch.
set(_SFRAW_LIBRAW_PATCHED_TREE_SHA256_23
    "d1fd81838e54c83a608f91988cb5e00035891aeab1248bd92aa68b2f12007f77")
# Exact aggregate shipped by the immediately preceding 22-patch resolver.
# It is accepted only as a migration source for the modification-notice patch.
set(_SFRAW_LIBRAW_PATCHED_TREE_SHA256_22
    "2fb59481a23597b07cb7fd4019b15cba8b8086bbc2514253384714ab0b9fb742")
# Exact aggregate shipped by the immediately preceding 21-patch resolver. It
# is accepted only as a migration source: overlapping older patches are not all
# independently reverse-applicable after later patches have changed context.
set(_SFRAW_LIBRAW_PATCHED_TREE_SHA256_21
    "5ef9f7b77b5ef38d93e5cbebad6c0aa30bdfc66fc801a766668dcc24e4f5535b")
set(_SFRAW_LIBRAW_PATCHES
    "${_SFRAW_LIBRAW_PATCH_DIR}/0001-openmp-wavelet-initialize-size.patch"
    "${_SFRAW_LIBRAW_PATCH_DIR}/0002-newsubfiletype-unsigned.patch"
    "${_SFRAW_LIBRAW_PATCH_DIR}/0003-ljpeg-zero-category.patch"
    "${_SFRAW_LIBRAW_PATCH_DIR}/0004-bound-tiff-metadata-allocations.patch"
    "${_SFRAW_LIBRAW_PATCH_DIR}/0005-bound-lossless-jpeg-work.patch"
    "${_SFRAW_LIBRAW_PATCH_DIR}/0006-bound-dng-tile-streams.patch"
    "${_SFRAW_LIBRAW_PATCH_DIR}/0007-bound-sony-ljpeg-work.patch"
    "${_SFRAW_LIBRAW_PATCH_DIR}/0008-bound-ljpeg-segments.patch"
    "${_SFRAW_LIBRAW_PATCH_DIR}/0009-bound-ljpeg-setup-work.patch"
    "${_SFRAW_LIBRAW_PATCH_DIR}/0010-harden-hasselblad-ljpeg.patch"
    "${_SFRAW_LIBRAW_PATCH_DIR}/0011-harden-ljpeg-idct.patch"
    "${_SFRAW_LIBRAW_PATCH_DIR}/0012-harden-cr2-slice-arithmetic.patch"
    "${_SFRAW_LIBRAW_PATCH_DIR}/0013-harden-canon-sraw.patch"
    "${_SFRAW_LIBRAW_PATCH_DIR}/0014-bound-identify-ljpeg-work.patch"
    "${_SFRAW_LIBRAW_PATCH_DIR}/0015-bound-fp-dng-compressed-work.patch"
    "${_SFRAW_LIBRAW_PATCH_DIR}/0016-bound-canon-sraw-white-balance.patch"
    "${_SFRAW_LIBRAW_PATCH_DIR}/0017-make-ljpeg-idct-init-thread-safe.patch"
    "${_SFRAW_LIBRAW_PATCH_DIR}/0018-bound-identify-maximum-shift.patch"
    "${_SFRAW_LIBRAW_PATCH_DIR}/0019-bound-hasselblad-predictor-arithmetic.patch"
    "${_SFRAW_LIBRAW_PATCH_DIR}/0020-bound-olympus-metadata-and-arithmetic.patch"
    "${_SFRAW_LIBRAW_PATCH_DIR}/0021-harden-panasonic-c8-decoder.patch"
    "${_SFRAW_LIBRAW_PATCH_DIR}/0022-bound-fixed-header-string-reads.patch"
    "${_SFRAW_LIBRAW_PATCH_DIR}/0023-record-local-modification-notices.patch"
    "${_SFRAW_LIBRAW_PATCH_DIR}/0024-define-xtrans-negative-index-arithmetic.patch"
    "${_SFRAW_LIBRAW_PATCH_DIR}/0025-define-icc-s15fixed16-conversion.patch")

# Multi-config generators can emit a shipping configuration even though
# CMAKE_BUILD_TYPE is empty. Treat them as shipping-capable and normalize the
# single-config spelling so case cannot bypass release-only provenance gates.
function(sfraw_is_shipping_config out_var)
    string(TOLOWER "${CMAKE_BUILD_TYPE}" _build_type_lower)
    if (NOT CMAKE_CONFIGURATION_TYPES AND _build_type_lower STREQUAL "debug")
        set(${out_var} FALSE PARENT_SCOPE)
    else ()
        set(${out_var} TRUE PARENT_SCOPE)
    endif ()
endfunction()

function(_sfraw_verify_file_sha256 file_path expected_sha256 description)
    if (NOT EXISTS "${file_path}")
        message(FATAL_ERROR "sfraw: missing ${description}: ${file_path}")
    endif ()
    file(SHA256 "${file_path}" _actual_sha256)
    if (NOT _actual_sha256 STREQUAL expected_sha256)
        message(FATAL_ERROR
            "sfraw: ${description} SHA256 mismatch: expected ${expected_sha256}, "
            "got ${_actual_sha256}")
    endif ()
endfunction()

function(_sfraw_apply_patch source_dir patch_file)
    # `git apply` walks up to the parent repository and silently skips files in
    # FetchContent's extracted, untracked tree. Use the ordinary patch utility,
    # which is rooted strictly at WORKING_DIRECTORY. Git for Windows bundles it
    # under usr/bin even though that directory is not normally on PATH.
    find_package(Git QUIET)
    if (GIT_EXECUTABLE)
        get_filename_component(_git_command_dir "${GIT_EXECUTABLE}" DIRECTORY)
        get_filename_component(_git_install_dir "${_git_command_dir}/.." ABSOLUTE)
    endif ()
    find_program(_patch_executable
        NAMES patch patch.exe
        HINTS "${_git_install_dir}/usr/bin"
        REQUIRED)
    execute_process(
        COMMAND "${_patch_executable}" --dry-run --batch --forward -p1
                -i "${patch_file}"
        WORKING_DIRECTORY "${source_dir}"
        RESULT_VARIABLE _can_apply
        OUTPUT_VARIABLE _apply_out
        ERROR_VARIABLE _apply_err)
    if (_can_apply EQUAL 0)
        execute_process(
            COMMAND "${_patch_executable}" --batch --forward
                    --no-backup-if-mismatch -p1
                    -i "${patch_file}"
            WORKING_DIRECTORY "${source_dir}"
            RESULT_VARIABLE _apply_rc
            OUTPUT_VARIABLE _apply_out
            ERROR_VARIABLE _apply_err)
        if (NOT _apply_rc EQUAL 0)
            message(FATAL_ERROR
                "sfraw: failed to apply ${patch_file}: ${_apply_out}${_apply_err}")
        endif ()
        return()
    endif ()

    execute_process(
        COMMAND "${_patch_executable}" --dry-run --batch --reverse -p1
                -i "${patch_file}"
        WORKING_DIRECTORY "${source_dir}"
        RESULT_VARIABLE _already_applied
        OUTPUT_QUIET ERROR_QUIET)
    if (NOT _already_applied EQUAL 0)
        message(FATAL_ERROR
            "sfraw: ${patch_file} applies neither forward nor reverse. "
            "The LibRaw source is not the reviewed 0.22.2 baseline.")
    endif ()
endfunction()

function(_sfraw_assert_contains file_path regex description)
    if (NOT EXISTS "${file_path}")
        message(FATAL_ERROR "sfraw: missing ${description}: ${file_path}")
    endif ()
    file(READ "${file_path}" _contents)
    if (NOT _contents MATCHES "${regex}")
        message(FATAL_ERROR "sfraw: ${description} verification failed in ${file_path}")
    endif ()
endfunction()

function(_sfraw_tree_has_expected_aggregate source_dir expected_sha256 out_var)
    file(GLOB_RECURSE _candidate_files
        LIST_DIRECTORIES FALSE
        RELATIVE "${source_dir}"
        "${source_dir}/src/*.cpp"
        "${source_dir}/libraw/*.h"
        "${source_dir}/internal/*.h")
    list(SORT _candidate_files)
    list(LENGTH _candidate_files _candidate_file_count)
    if (NOT _candidate_file_count EQUAL 100)
        set(${out_var} FALSE PARENT_SCOPE)
        return()
    endif ()
    set(_candidate_manifest "")
    foreach (_relative_path IN LISTS _candidate_files)
        file(SHA256 "${source_dir}/${_relative_path}" _file_sha256)
        string(APPEND _candidate_manifest
            "${_relative_path}:${_file_sha256}\n")
    endforeach ()
    string(SHA256 _candidate_sha256 "${_candidate_manifest}")
    if ("${_candidate_sha256}" STREQUAL "${expected_sha256}")
        set(${out_var} TRUE PARENT_SCOPE)
    else ()
        set(${out_var} FALSE PARENT_SCOPE)
    endif ()
endfunction()

function(_sfraw_verify_patched_tree source_dir)
    # URL_HASH authenticates the first extraction. This aggregate authenticates
    # every C++ source/header used by later incremental configurations after the
    # reviewed patch series, so a modified FetchContent `_deps` tree cannot ship
    # merely because CMake's download stamp already exists.
    file(GLOB_RECURSE _audited_files
        LIST_DIRECTORIES FALSE
        RELATIVE "${source_dir}"
        "${source_dir}/src/*.cpp"
        "${source_dir}/libraw/*.h"
        "${source_dir}/internal/*.h")
    list(SORT _audited_files)
    list(LENGTH _audited_files _audited_file_count)
    if (NOT _audited_file_count EQUAL 100)
        message(FATAL_ERROR
            "sfraw: patched LibRaw tree has ${_audited_file_count} audited files; "
            "expected 100")
    endif ()

    # A successful configure must not become a stale-cache bypass. Register
    # every audited source/header and every patch as a configure dependency so
    # an incremental build automatically reruns the SHA checks before it can
    # compile a modified FetchContent tree.
    set(_audited_absolute_files "")
    foreach (_relative_path IN LISTS _audited_files)
        list(APPEND _audited_absolute_files
            "${source_dir}/${_relative_path}")
    endforeach ()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${_audited_absolute_files} ${_SFRAW_LIBRAW_PATCHES})

    set(_tree_manifest "")
    foreach (_relative_path IN LISTS _audited_files)
        file(SHA256 "${source_dir}/${_relative_path}" _file_sha256)
        string(APPEND _tree_manifest
            "${_relative_path}:${_file_sha256}\n")
    endforeach ()
    string(SHA256 _tree_sha256 "${_tree_manifest}")
    if (NOT "${_tree_sha256}" STREQUAL
            "${_SFRAW_LIBRAW_PATCHED_TREE_SHA256}")
        message(FATAL_ERROR
            "sfraw: patched LibRaw source/header aggregate mismatch: "
            "${_tree_sha256}")
    endif ()
endfunction()

function(sfraw_resolve_libraw out_var)
    _sfraw_verify_file_sha256(
        "${_SFRAW_LIBRAW_PATCH_DIR}/0001-openmp-wavelet-initialize-size.patch"
        "b73a81a79a918d76ffb29f5272cac877cd31e4c1c6d039fe70b32d31ccfb4de6"
        "OpenMP wavelet patch")
    _sfraw_verify_file_sha256(
        "${_SFRAW_LIBRAW_PATCH_DIR}/0002-newsubfiletype-unsigned.patch"
        "84c77a94b904944340c8921e6b8f47600fa733a30376f08207e3376b06a8e7cd"
        "NewSubfileType patch")
    _sfraw_verify_file_sha256(
        "${_SFRAW_LIBRAW_PATCH_DIR}/0003-ljpeg-zero-category.patch"
        "4330ad0c8ef7a996cbbb75a227c1c71c98c7f2e46986bcac98e02660bb8e4010"
        "lossless-JPEG category patch")
    _sfraw_verify_file_sha256(
        "${_SFRAW_LIBRAW_PATCH_DIR}/0004-bound-tiff-metadata-allocations.patch"
        "4546199834ebeb8cbfbd2ad3613afff7a7f8e9fa8bf38be6994a5688868e2caf"
        "bounded TIFF metadata allocation patch")
    _sfraw_verify_file_sha256(
        "${_SFRAW_LIBRAW_PATCH_DIR}/0005-bound-lossless-jpeg-work.patch"
        "d10a59f884e652c3764e793c2a5ebcc56760c07538f1fbd6a0bdefdd760abdb0"
        "bounded lossless-JPEG work patch")
    _sfraw_verify_file_sha256(
        "${_SFRAW_LIBRAW_PATCH_DIR}/0006-bound-dng-tile-streams.patch"
        "86221caa689460fbd04e25ef93cc3d87b7450b17b6b5843a28c4b984994ac4c2"
        "bounded DNG tile-stream patch")
    _sfraw_verify_file_sha256(
        "${_SFRAW_LIBRAW_PATCH_DIR}/0007-bound-sony-ljpeg-work.patch"
        "45d4735a5e725deabd414f8630db91587dac09329011d546ecd307fe329b1274"
        "bounded Sony lossless-JPEG work patch")
    _sfraw_verify_file_sha256(
        "${_SFRAW_LIBRAW_PATCH_DIR}/0008-bound-ljpeg-segments.patch"
        "bcbedeefb69b864ae888040b4ae31174e95dcc39e6800852acdb5d7192f3bd39"
        "bounded lossless-JPEG segment parser patch")
    _sfraw_verify_file_sha256(
        "${_SFRAW_LIBRAW_PATCH_DIR}/0009-bound-ljpeg-setup-work.patch"
        "54bfc7b45935f23dc92fa7035236358c5c39b63ea1c82d4c626fd7d95a7d3a5a"
        "bounded lossless-JPEG setup-work patch")
    _sfraw_verify_file_sha256(
        "${_SFRAW_LIBRAW_PATCH_DIR}/0010-harden-hasselblad-ljpeg.patch"
        "3aaf0f17318367096e13e0c3436f095e025c6555b75a5537416fd42830c2b378"
        "Hasselblad lossless-JPEG contract patch")
    _sfraw_verify_file_sha256(
        "${_SFRAW_LIBRAW_PATCH_DIR}/0011-harden-ljpeg-idct.patch"
        "b7a267feecd93ab98bc52f1d4f7e1d2901ca500059b01e89eee46b03077df7e6"
        "lossless-JPEG IDCT arithmetic patch")
    _sfraw_verify_file_sha256(
        "${_SFRAW_LIBRAW_PATCH_DIR}/0012-harden-cr2-slice-arithmetic.patch"
        "977105fb62a9ccfe6f49295fa79fee33c4aeb2273b90df8ffab617e95d3f7b82"
        "CR2Slice arithmetic patch")
    _sfraw_verify_file_sha256(
        "${_SFRAW_LIBRAW_PATCH_DIR}/0013-harden-canon-sraw.patch"
        "423eb01e16c51d72fccd8284dd063ea69dea38b0ac8aed255bea37f1875f6575"
        "Canon sRAW contract and arithmetic patch")
    _sfraw_verify_file_sha256(
        "${_SFRAW_LIBRAW_PATCH_DIR}/0014-bound-identify-ljpeg-work.patch"
        "613a2039f9d97ea3e0a7c915f233f1a9b5cf23134b612a73a5c1b4597b9db31a"
        "identify-stage lossless-JPEG work patch")
    _sfraw_verify_file_sha256(
        "${_SFRAW_LIBRAW_PATCH_DIR}/0015-bound-fp-dng-compressed-work.patch"
        "03947dd32d72c2e578c7d727ef5d2a74af9f7e340245e56b883f127aeafb3395"
        "floating-point DNG compressed-work patch")
    _sfraw_verify_file_sha256(
        "${_SFRAW_LIBRAW_PATCH_DIR}/0016-bound-canon-sraw-white-balance.patch"
        "a0333dec464b105c45b1802a742e95582704efcba8c15e7f0d6871153dd93546"
        "Canon sRAW white-balance patch")
    _sfraw_verify_file_sha256(
        "${_SFRAW_LIBRAW_PATCH_DIR}/0017-make-ljpeg-idct-init-thread-safe.patch"
        "7ac3ae1e6c36d7f862026e5be036d1456fdf6b87b4eac3f920022c42c13c7522"
        "lossless-JPEG IDCT initialization patch")
    _sfraw_verify_file_sha256(
        "${_SFRAW_LIBRAW_PATCH_DIR}/0018-bound-identify-maximum-shift.patch"
        "994cbbb5988f66fd9331550cc99bfe1b9ddc92db20c99a32d32dcccf7bb7bc18"
        "identify maximum-shift arithmetic patch")
    _sfraw_verify_file_sha256(
        "${_SFRAW_LIBRAW_PATCH_DIR}/0019-bound-hasselblad-predictor-arithmetic.patch"
        "088744bd7d349b0fbc0eecfc7933aad3f0cac49f95e2a0d48c8896a758f740c7"
        "Hasselblad predictor arithmetic patch")
    _sfraw_verify_file_sha256(
        "${_SFRAW_LIBRAW_PATCH_DIR}/0020-bound-olympus-metadata-and-arithmetic.patch"
        "b28ebf2e312a09cbf7b2b6969b289cee7b6dd74ec67ea70ee29ca531cc61bcf2"
        "Olympus metadata and predictor arithmetic patch")
    _sfraw_verify_file_sha256(
        "${_SFRAW_LIBRAW_PATCH_DIR}/0021-harden-panasonic-c8-decoder.patch"
        "d528a6399ddcc9e14e07a0d76c9bc01523ce0547ff81f2b8d927908d234da36b"
        "Panasonic C8 decoder contract patch")
    _sfraw_verify_file_sha256(
        "${_SFRAW_LIBRAW_PATCH_DIR}/0022-bound-fixed-header-string-reads.patch"
        "29fd07f40f82d34b1ffafd55f51e277d30ff3ecf22597c2d30dc4c45676695cf"
        "fixed-header string-read bounds patch")
    _sfraw_verify_file_sha256(
        "${_SFRAW_LIBRAW_PATCH_DIR}/0023-record-local-modification-notices.patch"
        "3d8d2eef4f59ef665d58fede9ae780d523deb5a119c6c67ed6844ad4812edc38"
        "local modification-notice patch")
    _sfraw_verify_file_sha256(
        "${_SFRAW_LIBRAW_PATCH_DIR}/0024-define-xtrans-negative-index-arithmetic.patch"
        "c6dbbe4707503049e601b993c5238b8e90404ff0d442d5c334c98f649f28f5c3"
        "defined X-Trans negative-index arithmetic patch")
    _sfraw_verify_file_sha256(
        "${_SFRAW_LIBRAW_PATCH_DIR}/0025-define-icc-s15fixed16-conversion.patch"
        "895b644886247bb3853002c27aa5d56475429c05f62ec556360a036dc8f3a9a4"
        "defined ICC s15Fixed16 conversion patch")

    sfraw_is_shipping_config(_shipping_config)
    if (_shipping_config)
        if (SFRAW_LIBRAW_SOURCE_DIR)
            message(FATAL_ERROR
                "sfraw: SFRAW_LIBRAW_SOURCE_DIR is debug/offline-only. "
                "Shipping builds must use the official URL+SHA256 archive.")
        endif ()
        if (DEFINED FETCHCONTENT_SOURCE_DIR_SFRAW_LIBRAW_0_22_2 AND
                NOT FETCHCONTENT_SOURCE_DIR_SFRAW_LIBRAW_0_22_2 STREQUAL "")
            message(FATAL_ERROR
                "sfraw: FetchContent source overrides are forbidden in shipping "
                "builds; use the official URL+SHA256 archive")
        endif ()
        if (FETCHCONTENT_FULLY_DISCONNECTED)
            message(FATAL_ERROR
                "sfraw: FETCHCONTENT_FULLY_DISCONNECTED is forbidden in shipping "
                "builds; archive provenance must remain enforceable")
        endif ()
    endif ()

    if (SFRAW_LIBRAW_SOURCE_DIR)
        get_filename_component(_source_dir "${SFRAW_LIBRAW_SOURCE_DIR}" ABSOLUTE)
        message(STATUS "sfraw: using verified debug/offline source ${_source_dir}")
    else ()
        include(FetchContent)
        message(STATUS
            "sfraw: fetching LibRaw ${SFRAW_PINNED_LIBRAW_VERSION} from "
            "${SFRAW_PINNED_LIBRAW_URL}")
        # Version-stamped name prevents an existing 0.21.x FetchContent tree from
        # being reused after this upgrade. Prefer extraction-time timestamps when
        # supported so URL changes cannot leave stale build products; Android's
        # pinned CMake 3.22 predates this FetchContent option.
        set(_fetch_timestamp_args)
        if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24")
            list(APPEND _fetch_timestamp_args DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        endif ()
        FetchContent_Declare(
            sfraw_libraw_0_22_2
            URL "${SFRAW_PINNED_LIBRAW_URL}"
            URL_HASH "SHA256=${SFRAW_PINNED_LIBRAW_SHA256}"
            ${_fetch_timestamp_args})
        FetchContent_GetProperties(sfraw_libraw_0_22_2)
        if (NOT sfraw_libraw_0_22_2_POPULATED)
            FetchContent_Populate(sfraw_libraw_0_22_2)
        endif ()
        set(_source_dir "${sfraw_libraw_0_22_2_SOURCE_DIR}")
    endif ()

    if (NOT EXISTS "${_source_dir}/libraw/libraw.h")
        message(FATAL_ERROR
            "sfraw: LibRaw source is absent at ${_source_dir}; refusing a decoder stub")
    endif ()

    _sfraw_tree_has_expected_aggregate(
        "${_source_dir}" "${_SFRAW_LIBRAW_PATCHED_TREE_SHA256}"
        _already_patched)
    if (_already_patched)
        message(STATUS "sfraw: existing LibRaw tree matches the audited aggregate")
    else ()
        _sfraw_tree_has_expected_aggregate(
            "${_source_dir}" "${_SFRAW_LIBRAW_PATCHED_TREE_SHA256_24}"
            _is_exact_24_patch_tree)
        if (_is_exact_24_patch_tree)
            message(STATUS
                "sfraw: migrating exact audited 24-patch tree to patch 25")
            _sfraw_apply_patch(
                "${_source_dir}"
                "${_SFRAW_LIBRAW_PATCH_DIR}/0025-define-icc-s15fixed16-conversion.patch")
        else ()
            _sfraw_tree_has_expected_aggregate(
                "${_source_dir}" "${_SFRAW_LIBRAW_PATCHED_TREE_SHA256_23}"
                _is_exact_23_patch_tree)
            if (_is_exact_23_patch_tree)
                message(STATUS
                    "sfraw: migrating exact audited 23-patch tree to patches 24-25")
                _sfraw_apply_patch(
                    "${_source_dir}"
                    "${_SFRAW_LIBRAW_PATCH_DIR}/0024-define-xtrans-negative-index-arithmetic.patch")
                _sfraw_apply_patch(
                    "${_source_dir}"
                    "${_SFRAW_LIBRAW_PATCH_DIR}/0025-define-icc-s15fixed16-conversion.patch")
            else ()
                _sfraw_tree_has_expected_aggregate(
                    "${_source_dir}" "${_SFRAW_LIBRAW_PATCHED_TREE_SHA256_22}"
                    _is_exact_22_patch_tree)
                if (_is_exact_22_patch_tree)
                    message(STATUS
                        "sfraw: migrating exact audited 22-patch tree to patches 23-25")
                    _sfraw_apply_patch(
                        "${_source_dir}"
                        "${_SFRAW_LIBRAW_PATCH_DIR}/0023-record-local-modification-notices.patch")
                    _sfraw_apply_patch(
                        "${_source_dir}"
                        "${_SFRAW_LIBRAW_PATCH_DIR}/0024-define-xtrans-negative-index-arithmetic.patch")
                    _sfraw_apply_patch(
                        "${_source_dir}"
                        "${_SFRAW_LIBRAW_PATCH_DIR}/0025-define-icc-s15fixed16-conversion.patch")
                else ()
                    _sfraw_tree_has_expected_aggregate(
                        "${_source_dir}" "${_SFRAW_LIBRAW_PATCHED_TREE_SHA256_21}"
                        _is_exact_21_patch_tree)
                    if (_is_exact_21_patch_tree)
                        message(STATUS
                            "sfraw: migrating exact audited 21-patch tree to patches 22-25")
                        _sfraw_apply_patch(
                            "${_source_dir}"
                            "${_SFRAW_LIBRAW_PATCH_DIR}/0022-bound-fixed-header-string-reads.patch")
                        _sfraw_apply_patch(
                            "${_source_dir}"
                            "${_SFRAW_LIBRAW_PATCH_DIR}/0023-record-local-modification-notices.patch")
                        _sfraw_apply_patch(
                            "${_source_dir}"
                            "${_SFRAW_LIBRAW_PATCH_DIR}/0024-define-xtrans-negative-index-arithmetic.patch")
                        _sfraw_apply_patch(
                            "${_source_dir}"
                            "${_SFRAW_LIBRAW_PATCH_DIR}/0025-define-icc-s15fixed16-conversion.patch")
                    else ()
                        foreach (_patch IN LISTS _SFRAW_LIBRAW_PATCHES)
                            if (NOT EXISTS "${_patch}")
                                message(FATAL_ERROR
                                    "sfraw: required local patch is missing: ${_patch}")
                            endif ()
                            _sfraw_apply_patch("${_source_dir}" "${_patch}")
                        endforeach ()
                    endif ()
                endif ()
            endif ()
        endif ()
    endif ()

    file(GLOB_RECURSE _patch_artifacts
        LIST_DIRECTORIES FALSE
        "${_source_dir}/*.orig"
        "${_source_dir}/*.rej")
    if (_patch_artifacts)
        list(JOIN _patch_artifacts ", " _patch_artifact_list)
        message(FATAL_ERROR
            "sfraw: patch backup/reject artifacts are forbidden: "
            "${_patch_artifact_list}")
    endif ()

    _sfraw_assert_contains(
        "${_source_dir}/libraw/libraw_version.h"
        "#define LIBRAW_MAJOR_VERSION[ \t]+0"
        "LibRaw major version")
    _sfraw_assert_contains(
        "${_source_dir}/libraw/libraw_version.h"
        "#define LIBRAW_MINOR_VERSION[ \t]+22"
        "LibRaw minor version")
    _sfraw_assert_contains(
        "${_source_dir}/libraw/libraw_version.h"
        "#define LIBRAW_PATCH_VERSION[ \t]+2"
        "LibRaw patch version")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/decoders_dcraw.cpp"
        "\\(\\(unsigned\\)col < raw_width\\)"
        "CVE-2026-21413 hostile-column guard")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/decoders_dcraw.cpp"
        "if \\(len == 0\\)[\r\n \t]+return 0;"
        "lossless-JPEG zero-category guard")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/decoders_dcraw.cpp"
        "jpeg_samples > geometry_limit"
        "lossless-JPEG decoded-sample work budget")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/dng.cpp"
        "jpeg_samples > remaining_ljpeg_samples"
        "lossless-DNG cumulative decoded-sample work budget")
    _sfraw_assert_contains(
        "${_source_dir}/src/metadata/identify.cpp"
        "tiff_bps >= 16 [?] 0xffffU"
        "identify maximum-shift bound")
    _sfraw_assert_contains(
        "${_source_dir}/src/metadata/identify.cpp"
        "tiff_bps >= 12 && tiff_bps <= 16"
        "Sony default-black shift bound")
    _sfraw_assert_contains(
        "${_source_dir}/src/metadata/identify.cpp"
        "tiff_bps < 7 \\|\\| tiff_bps > 16"
        "Samsung default-black shift bound")
    _sfraw_assert_contains(
        "${_source_dir}/src/metadata/identify.cpp"
        "tiff_ifd\\[iifd\\]\\.bps <= 16"
        "DNG default-WhiteLevel shift bound")
    _sfraw_assert_contains(
        "${_source_dir}/src/metadata/identify.cpp"
        "memcmp\\(head, \"PXN\", sizeof\\(\"PXN\"\\)\\)"
        "bounded PXN identify signature")
    _sfraw_assert_contains(
        "${_source_dir}/src/metadata/identify.cpp"
        "memcmp\\(head, \"qktk\", sizeof\\(\"qktk\"\\)\\)"
        "bounded QuickTake 100 identify signature")
    _sfraw_assert_contains(
        "${_source_dir}/src/metadata/identify.cpp"
        "memcmp\\(head, \"qktn\", sizeof\\(\"qktn\"\\)\\)"
        "bounded QuickTake 150 identify signature")
    _sfraw_assert_contains(
        "${_source_dir}/src/metadata/makernotes.cpp"
        "char buf\\[11\\];"
        "MakerNote fixed-header NUL sentinel")
    _sfraw_assert_contains(
        "${_source_dir}/src/x3f/x3f_parse_process.cpp"
        "lr_memmem\\(buf, bytes_read, \"SIGMA dp\", 8\\)"
        "bounded X3F model signature search")
    _sfraw_assert_contains(
        "${_source_dir}/src/x3f/x3f_parse_process.cpp"
        "fnd && fnd [+] 8 < buffer_end"
        "bounded X3F model-selector read")
    _sfraw_assert_contains(
        "${_source_dir}/src/x3f/x3f_parse_process.cpp"
        "MIN\\(size_t\\(20\\), remaining\\)"
        "bounded X3F Quattro model search")
    _sfraw_assert_contains(
        "${_source_dir}/src/metadata/tiff.cpp"
        "tiff_bps <= 16"
        "TIFF default-maximum shift bound")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/dng.cpp"
        "tile_streams_across > permitted_tile_streams / tile_streams_down"
        "lossless-DNG tile-stream work budget")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/fp_dng.cpp"
        "tileCnt64 > permittedTileCnt"
        "deflate/float-DNG tile-stream work budget")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/decoders_libraw_dcrdefs.cpp"
        "tile_streams_across > permitted_tile_streams / tile_streams_down"
        "packed-DNG tile-stream work budget")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/decoders_libraw_dcrdefs.cpp"
        "jpeg_samples > remaining_ljpeg_samples"
        "Sony lossless-JPEG cumulative sample budget")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/decoders_libraw_dcrdefs.cpp"
        "remaining_tile_streams < 1"
        "Sony lossless-JPEG tile-stream work budget")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/decoders_libraw_dcrdefs.cpp"
        "jh.clrs != 4"
        "Sony lossless-JPEG component contract")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/decoders_dcraw.cpp"
        "dht_end - probe < 16"
        "lossless-JPEG Huffman count-table bounds")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/decoders_dcraw.cpp"
        "available_codes < 0"
        "lossless-JPEG Huffman code-space bound")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/decoders_dcraw.cpp"
        "leaves > 256"
        "lossless-JPEG Huffman symbol-count bound")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/decoders_dcraw.cpp"
        "jh->bits < 1"
        "lossless-JPEG positive precision contract")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/decoders_dcraw.cpp"
        "FORC\\(20\\)"
        "lossless-JPEG complete Huffman cleanup")
    _sfraw_assert_contains(
        "${_source_dir}/libraw/libraw_internal.h"
        "unsigned setup_work_units;"
        "lossless-JPEG setup-work accounting field")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/decoders_dcraw.cpp"
        "segment_length \\+ 2U"
        "lossless-JPEG marker-work accounting")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/dng.cpp"
        "remaining_ljpeg_setup_units -= jh.setup_work_units"
        "DNG lossless-JPEG cumulative setup-work budget")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/decoders_libraw_dcrdefs.cpp"
        "remaining_ljpeg_setup_units -= jh.setup_work_units"
        "Sony lossless-JPEG cumulative setup-work budget")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/load_mfbacks.cpp"
        "raw_width < 2 || \\(raw_width & 1\\)"
        "Hasselblad even-width predictor contract")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/load_mfbacks.cpp"
        "\\(unsigned\\)len\\[c\\] > 16"
        "Hasselblad Huffman category contract")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/load_mfbacks.cpp"
        "next_pred < INT_MIN \\|\\| next_pred > INT_MAX"
        "Hasselblad predictor arithmetic bound")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/olympus14.cpp"
        "UINT64 [*]unary_work_remaining"
        "Olympus unary decoder-work budget")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/olympus14.cpp"
        "pixel_final_value < 0 \\|\\| pixel_final_value > USHRT_MAX"
        "Olympus checked predictor scaling")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/olympus14.cpp"
        "oly[.]ValidBits > 16"
        "Olympus 16-bit metadata bound")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/pana8.cpp"
        "meta[.]tag40_count != 17 \\|\\| meta[.]tag41_count != 17"
        "Panasonic C8 Huffman metadata contract")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/pana8.cpp"
        "readbytes != requested"
        "Panasonic C8 exact stripe reads")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/pana8.cpp"
        "huff_index >= 17"
        "Panasonic C8 malformed-prefix rejection")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/pana8.cpp"
        "symbol_bits > UINT64\\(bufio[.]bitSize\\(\\)\\) - consumed_bits"
        "Panasonic C8 exact compressed-bit budget")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/pana8.cpp"
        "stripe_field_count\\[field\\] != pana8[.]stripe_count"
        "Panasonic C8 complete stripe metadata")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/pana8.cpp"
        "v38 >> \\(64U - v90\\)"
        "Panasonic C8 defined extra-bit extraction")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/pana8.cpp"
        "parallel for reduction\\([+][:]errs\\)"
        "Panasonic C8 OpenMP error reduction")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/decoders_dcraw.cpp"
        "const INT64 dc = INT64"
        "lossless-JPEG IDCT checked DC predictor")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/decoders_dcraw.cpp"
        "\\(unsigned\\)i >= 64"
        "lossless-JPEG IDCT coefficient bound")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/decoders_dcraw.cpp"
        "const INT64 slice_area"
        "CR2Slice checked-width arithmetic")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/decoders_dcraw.cpp"
        "jrow < 0 \\|\\| jrow >= jh->high"
        "lossless-JPEG row-count contract")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/decoders_dcraw.cpp"
        "ecol <= scol"
        "Canon sRAW empty-slice termination")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/decoders_dcraw.cpp"
        "const INT64 scaledPixel"
        "Canon sRAW checked pixel scaling")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/decoders_dcraw.cpp"
        "INT64 versionParts\\[3\\]"
        "Canon sRAW bounded firmware-version parse")
    _sfraw_assert_contains(
        "${_source_dir}/src/metadata/identify.cpp"
        "ljpeg_setup_work_remaining = MIN"
        "identify-stage lossless-JPEG work budget")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/decoders_dcraw.cpp"
        "UINT64\\(units\\) > ljpeg_setup_work_remaining"
        "lossless-JPEG identify-work debit")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/fp_dng.cpp"
        "cumulativeBytes > compressedWorkBudget"
        "floating-point DNG cumulative compressed-work budget")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/fp_dng.cpp"
        "tileWorkingBytes > memoryLimit - raw_bytes"
        "floating-point DNG aggregate allocation budget")
    _sfraw_assert_contains(
        "${_source_dir}/src/metadata/canon.cpp"
        "scaled_sraw_mul <= \\(float\\)USHRT_MAX"
        "Canon sRAW white-balance conversion bound")
    _sfraw_assert_contains(
        "${_source_dir}/src/decoders/decoders_dcraw.cpp"
        "static const std::vector<float> cs"
        "thread-safe lossless-JPEG IDCT table initialization")
    _sfraw_assert_contains(
        "${_source_dir}/src/metadata/tiff.cpp"
        "newsubfiletype = get4\\(\\);"
        "NewSubfileType unsigned parse")
    _sfraw_assert_contains(
        "${_source_dir}/libraw/libraw_const.h"
        "LIBRAW_MAX_METADATA_ALLOC_SIZE_MB"
        "identify-phase metadata allocation budget")
    _sfraw_assert_contains(
        "${_source_dir}/src/metadata/tiff.cpp"
        "if \\(rawopcode.data\\)"
        "duplicate DNG opcode rejection")
    _sfraw_assert_contains(
        "${_source_dir}/src/demosaic/xtrans_demosaic.cpp"
        "rix\\[-\\(i << c\\)\\]"
        "defined X-Trans negative index arithmetic")
    _sfraw_assert_contains(
        "${_source_dir}/src/postprocessing/postprocessing_utils_dcrdefs.cpp"
        "static_cast<unsigned>\\(signed_fixed\\)"
        "defined ICC s15Fixed16 signed conversion")

    set(_sfraw_locally_modified_files
        "internal/var_defines.h"
        "libraw/libraw_const.h"
        "libraw/libraw_internal.h"
        "src/decoders/decoders_dcraw.cpp"
        "src/decoders/decoders_libraw_dcrdefs.cpp"
        "src/decoders/dng.cpp"
        "src/decoders/fp_dng.cpp"
        "src/decoders/load_mfbacks.cpp"
        "src/decoders/olympus14.cpp"
        "src/decoders/pana8.cpp"
        "src/metadata/canon.cpp"
        "src/metadata/identify.cpp"
        "src/metadata/makernotes.cpp"
        "src/metadata/tiff.cpp"
        "src/postprocessing/postprocessing_aux.cpp"
        "src/utils/utils_libraw.cpp"
        "src/x3f/x3f_parse_process.cpp")
    foreach (_modified_file IN LISTS _sfraw_locally_modified_files)
        _sfraw_assert_contains(
            "${_source_dir}/${_modified_file}"
            "Modified by Spektrafilm Android contributors, 2026-08-30"
            "dated local modification notice")
    endforeach ()
    _sfraw_assert_contains(
        "${_source_dir}/src/demosaic/xtrans_demosaic.cpp"
        "Modified by Spektrafilm Android contributors, 2026-09-01"
        "dated X-Trans local modification notice")
    _sfraw_assert_contains(
        "${_source_dir}/src/postprocessing/postprocessing_utils_dcrdefs.cpp"
        "Modified by Spektrafilm Android contributors, 2026-09-01"
        "dated ICC conversion local modification notice")

    file(READ "${_source_dir}/src/postprocessing/postprocessing_aux.cpp" _wavelet)
    # Do not include the trailing semicolon in the match: CMake treats it as a
    # list separator and would count each source hit twice.
    string(REGEX MATCHALL "size = iheight \\* iwidth" _size_initializers "${_wavelet}")
    list(LENGTH _size_initializers _size_initializer_count)
    if (NOT _size_initializer_count EQUAL 2)
        message(FATAL_ERROR
            "sfraw: expected both serial and OpenMP wavelet size initializers; "
            "found ${_size_initializer_count}")
    endif ()

    _sfraw_verify_patched_tree("${_source_dir}")

    set(${out_var} "${_source_dir}" PARENT_SCOPE)
endfunction()
