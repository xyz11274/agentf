import { test, expect } from '@playwright/test'

test.describe('Navigation', () => {
  test('homepage redirects to /packages', async ({ page }) => {
    await page.goto('/')
    await expect(page).toHaveURL(/\/packages/)
  })

  test('navigates to chat page via sidebar', async ({ page }) => {
    await page.goto('/')
    await page.click('text=Agent Q&A')
    await expect(page).toHaveURL(/\/chat/)
  })
})

test.describe('Package page', () => {
  test('shows empty state when no package selected', async ({ page }) => {
    await page.goto('/packages')
    await expect(page.locator('text=Select a file from the tree')).toBeVisible()
  })

  test('file tree loads for harness_agent package', async ({ page }) => {
    await page.goto('/packages/harness_agent')
    // Wait for the tree to render
    await page.waitForSelector('.el-tree', { timeout: 5000 }).catch(() => {})
    // Should show some file nodes
    const treeNodes = page.locator('.el-tree-node')
    const count = await treeNodes.count()
    expect(count).toBeGreaterThan(0)
  })
})

test.describe('Chat page', () => {
  test('shows empty state when no agent selected', async ({ page }) => {
    await page.goto('/chat')
    await expect(page.locator('text=Select an agent')).toBeVisible()
  })

  test('shows agent list in sidebar', async ({ page }) => {
    await page.goto('/chat')
    await page.waitForSelector('[data-testid="agent-list"]', { timeout: 5000 }).catch(() => {})
    const agentList = page.locator('[data-testid="agent-list"]')
    await expect(agentList).toBeVisible()
  })
})

test.describe('Daemon status', () => {
  test('daemon status indicator is visible', async ({ page }) => {
    await page.goto('/')
    await page.waitForSelector('[data-testid="daemon-status"]', { timeout: 5000 }).catch(() => {})
    // The banner might show connected or disconnected depending on daemon state
    const banner = page.locator('[data-testid="daemon-status"]')
    await expect(banner).toBeVisible()
  })
})
