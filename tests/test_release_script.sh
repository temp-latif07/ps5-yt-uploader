#!/usr/bin/env bash
# tests/test_release_script.sh
# Validates GitHub Actions release workflow syntax and release.sh execution.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKFLOW_FILE="${REPO_ROOT}/.github/workflows/release.yml"
RELEASE_SCRIPT="${REPO_ROOT}/scripts/release.sh"

echo "=========================================================="
echo " Running Release Workflow & Script Validation Tests"
echo "=========================================================="

# -------------------------------------------------------------
# Test 1: Validate .github/workflows/release.yml
# -------------------------------------------------------------
echo "[Test 1] Validating .github/workflows/release.yml..."

if [ ! -f "${WORKFLOW_FILE}" ]; then
    echo "FAIL: ${WORKFLOW_FILE} does not exist" >&2
    exit 1
fi

# Validate YAML syntax using ruby if available
if command -v ruby &>/dev/null; then
    ruby -e "require 'yaml'; YAML.load_file('${WORKFLOW_FILE}')" || {
        echo "FAIL: YAML syntax error in ${WORKFLOW_FILE}" >&2
        exit 1
    }
fi

# Check required triggers and inputs
grep -q "workflow_dispatch:" "${WORKFLOW_FILE}" || {
    echo "FAIL: workflow_dispatch trigger missing from ${WORKFLOW_FILE}" >&2
    exit 1
}

grep -q "tag:" "${WORKFLOW_FILE}" || {
    echo "FAIL: workflow_dispatch tag input missing from ${WORKFLOW_FILE}" >&2
    exit 1
}

grep -q "release_tag" "${WORKFLOW_FILE}" || {
    echo "FAIL: release_tag output resolution step missing from ${WORKFLOW_FILE}" >&2
    exit 1
}

grep -q "git pull --rebase origin main" "${WORKFLOW_FILE}" || {
    echo "FAIL: git pull --rebase origin main missing from ${WORKFLOW_FILE}" >&2
    exit 1
}

grep -q "v\*\.\*\.\*" "${WORKFLOW_FILE}" || {
    echo "FAIL: tag push trigger 'v*.*.*' missing from ${WORKFLOW_FILE}" >&2
    exit 1
}

# Check explicit permissions
grep -q "contents: write" "${WORKFLOW_FILE}" || {
    echo "FAIL: 'permissions: contents: write' missing from ${WORKFLOW_FILE}" >&2
    exit 1
}

# Check runner
grep -q "macos-latest" "${WORKFLOW_FILE}" || {
    echo "FAIL: runner macos-latest missing from ${WORKFLOW_FILE}" >&2
    exit 1
}

# Check actions/checkout with fetch-depth: 0
grep -q "actions/checkout" "${WORKFLOW_FILE}" || {
    echo "FAIL: actions/checkout step missing from ${WORKFLOW_FILE}" >&2
    exit 1
}
grep -q "fetch-depth: 0" "${WORKFLOW_FILE}" || {
    echo "FAIL: fetch-depth: 0 missing from checkout step in ${WORKFLOW_FILE}" >&2
    exit 1
}

# Check softprops/action-gh-release
grep -q "softprops/action-gh-release" "${WORKFLOW_FILE}" || {
    echo "FAIL: softprops/action-gh-release step missing from ${WORKFLOW_FILE}" >&2
    exit 1
}

# Check payload artifact names (ps5-yt-uploader.elf)
grep -q "ps5-yt-uploader\.elf" "${WORKFLOW_FILE}" || {
    echo "FAIL: ps5-yt-uploader.elf artifact missing from ${WORKFLOW_FILE}" >&2
    exit 1
}

# Check scripts/update_payload_repo.py invocation
grep -q "update_payload_repo\.py" "${WORKFLOW_FILE}" || {
    echo "FAIL: scripts/update_payload_repo.py invocation missing from ${WORKFLOW_FILE}" >&2
    exit 1
}

echo "  -> Release workflow YAML syntax and structure validated."

# -------------------------------------------------------------
# Test 2: Validate scripts/release.sh presence and permissions
# -------------------------------------------------------------
echo "[Test 2] Validating scripts/release.sh presence and permissions..."

if [ ! -f "${RELEASE_SCRIPT}" ]; then
    echo "FAIL: ${RELEASE_SCRIPT} does not exist" >&2
    exit 1
fi

if [ ! -x "${RELEASE_SCRIPT}" ]; then
    echo "FAIL: ${RELEASE_SCRIPT} is not executable" >&2
    exit 1
fi
echo "  -> scripts/release.sh is present and executable."

# -------------------------------------------------------------
# Test 3: Validate scripts/release.sh --dry-run execution
# -------------------------------------------------------------
echo "[Test 3] Testing scripts/release.sh --dry-run..."

# Record current tags to ensure dry-run does not create any tags
TAGS_BEFORE=$(git tag -l "v*")

if git diff-index --quiet HEAD --; then
    # Working tree is clean: test standard dry-run
    OUTPUT=$(bash "${RELEASE_SCRIPT}" --dry-run)
else
    # Tree currently has uncommitted modifications (e.g. pre-commit stage)
    # 1. Verify standard dry-run rejects the dirty working tree
    if bash "${RELEASE_SCRIPT}" --dry-run > /dev/null 2>&1; then
        echo "FAIL: scripts/release.sh did not reject dirty working tree" >&2
        exit 1
    fi
    echo "  -> Confirmed dirty working tree rejected by default."
    # 2. Run dry-run with --allow-dirty to execute the full test and build pipeline
    OUTPUT=$(bash "${RELEASE_SCRIPT}" --dry-run --allow-dirty)
fi

echo "${OUTPUT}"

TAGS_AFTER=$(git tag -l "v*")
if [ "${TAGS_BEFORE}" != "${TAGS_AFTER}" ]; then
    echo "FAIL: scripts/release.sh --dry-run created a git tag!" >&2
    exit 1
fi

# Output must report expected version 1.0.0
echo "${OUTPUT}" | grep -q "1.0.0" || {
    echo "FAIL: Release script did not report expected version 1.0.0" >&2
    exit 1
}

echo "${OUTPUT}" | grep -iq "dry.run" || {
    echo "FAIL: Release script did not indicate dry-run mode in output" >&2
    exit 1
}

echo "  -> scripts/release.sh --dry-run executed successfully."

# -------------------------------------------------------------
# Test 4: Validate dirty working tree rejection on clean tree
# -------------------------------------------------------------
if git diff-index --quiet HEAD --; then
    echo "[Test 4] Testing dirty working tree rejection by modifying a tracked file..."
    TEMP_TEST_FILE="${REPO_ROOT}/src/version.h.dirty_test_bak"
    cp "${REPO_ROOT}/src/version.h" "${TEMP_TEST_FILE}"

    # Temporarily dirty a tracked file
    echo "/* dirty test comment */" >> "${REPO_ROOT}/src/version.h"

    DIRTY_PASSED=0
    if bash "${RELEASE_SCRIPT}" --dry-run > /dev/null 2>&1; then
        DIRTY_PASSED=1
    fi

    # Restore version.h immediately
    cp "${TEMP_TEST_FILE}" "${REPO_ROOT}/src/version.h"
    rm -f "${TEMP_TEST_FILE}"

    if [ "${DIRTY_PASSED}" -eq 1 ]; then
        echo "FAIL: scripts/release.sh did not reject dirty working tree" >&2
        exit 1
    fi
    echo "  -> Dirty working tree correctly rejected."
else
    echo "[Test 4] Dirty working tree rejection already confirmed above in Test 3."
fi

# -------------------------------------------------------------
# Test 5: Validate non-interactive execution requires -y/--yes
# -------------------------------------------------------------
echo "[Test 5] Testing non-interactive execution safety check..."

NON_INT_ERR=$(echo "" | bash "${RELEASE_SCRIPT}" 2>&1 || true)
echo "${NON_INT_ERR}" | grep -q "Error: Non-interactive execution requires -y or --yes flag" || {
    echo "FAIL: release.sh did not require -y flag when executed without a TTY" >&2
    exit 1
}
echo "  -> Non-interactive execution correctly requires -y or --yes flag."

# -------------------------------------------------------------
# Test 6: Validate CHANGELOG.md and extract_release_notes.py
# -------------------------------------------------------------
echo "[Test 6] Testing CHANGELOG.md and release notes extractor..."

if [ ! -f "${REPO_ROOT}/CHANGELOG.md" ]; then
    echo "FAIL: CHANGELOG.md does not exist" >&2
    exit 1
fi

NOTES_OUTPUT=$(python3 "${REPO_ROOT}/scripts/extract_release_notes.py" "v1.0.0")
echo "${NOTES_OUTPUT}" | grep -q "Autonomous gameplay uploads directly from PS5 SSD to YouTube" || {
    echo "FAIL: extract_release_notes.py failed to extract 1.0.0 notes from CHANGELOG.md" >&2
    exit 1
}
echo "  -> Release notes successfully extracted from CHANGELOG.md."

echo "=========================================================="
echo "All Release Workflow & Script Validation Tests Passed Successfully!"
echo "=========================================================="
