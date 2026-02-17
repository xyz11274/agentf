#!/bin/bash

echo "🔍 Verifying CI/CD Implementation..."
echo ""

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

TOTAL=0
PASSED=0
FAILED=0

# Get the project root directory
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

verify_file() {
    TOTAL=$((TOTAL+1))
    if [ -f "$1" ]; then
        echo -e "${GREEN}✓${NC} $1"
        PASSED=$((PASSED+1))
    else
        echo -e "${RED}✗${NC} $1"
        FAILED=$((FAILED+1))
    fi
}

verify_dir() {
    TOTAL=$((TOTAL+1))
    if [ -d "$1" ]; then
        echo -e "${GREEN}✓${NC} $1/"
        PASSED=$((PASSED+1))
    else
        echo -e "${RED}✗${NC} $1/"
        FAILED=$((FAILED+1))
    fi
}

echo "📁 Directory Structure"
verify_dir ".github"
verify_dir ".github/workflows"
verify_dir ".github/ISSUE_TEMPLATE"
echo ""

echo "🔄 GitHub Actions Workflows"
verify_file ".github/workflows/build.yml"
verify_file ".github/workflows/code-quality.yml"
verify_file ".github/workflows/tests.yml"
verify_file ".github/workflows/release.yml"
verify_file ".github/workflows/manual_build.yml"
verify_file ".github/workflows/badge_status.yml"
echo ""

echo "⚙️ Configuration Files"
verify_file ".github/CODEOWNERS"
verify_file ".github/dependabot.yml"
verify_file ".github/pull_request_template.md"
echo ""

echo "📋 Issue Templates"
verify_file ".github/ISSUE_TEMPLATE/bug_report.md"
verify_file ".github/ISSUE_TEMPLATE/feature_request.md"
verify_file ".github/ISSUE_TEMPLATE/performance.md"
verify_file ".github/ISSUE_TEMPLATE/config.md"
echo ""

echo "📚 Documentation Files"
verify_file ".github/README.md"
verify_file ".github/CI_CD.md"
verify_file ".github/SETUP.md"
verify_file "CONTRIBUTING.md"
verify_file "README.md"
verify_file "TODO.md"
verify_file "docs/README.md"
verify_file "docs/CONTRIBUTING.md"
verify_file "docs/CI_CD_PIPELINE.md"
verify_file "docs/CI_CD_SETUP.md"
verify_file "docs/CI_CD_IMPLEMENTATION.md"
verify_file "docs/QUICK_REFERENCE.md"
echo ""

echo "📊 Summary"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Total checks:  $TOTAL"
echo -e "Passed:        ${GREEN}$PASSED${NC}"
echo -e "Failed:        ${RED}$FAILED${NC}"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}✓ All CI/CD components verified successfully!${NC}"
    echo ""
    echo "Next steps:"
    echo "1. Review docs/CI_CD_SETUP.md for configuration"
    echo "2. Configure branch protection rules in GitHub"
    echo "3. Enable Codecov integration"
    echo "4. Add status badges to README"
    echo ""
    exit 0
else
    echo -e "${RED}✗ Some components are missing!${NC}"
    exit 1
fi
