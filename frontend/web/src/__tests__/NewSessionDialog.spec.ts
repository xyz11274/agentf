import { describe, it, expect } from 'vitest'
import { mount } from '@vue/test-utils'
import NewSessionDialog from '@/components/NewSessionDialog.vue'

/**
 * Tests for NewSessionDialog.
 *
 * UI rendering of Element Plus components (el-dialog, el-input) is covered
 * by Playwright E2E tests.  These unit tests focus on the component's
 * JavaScript logic: event emission, ID generation, and default values.
 */
describe('NewSessionDialog', () => {
  it('emits create with provided client_id and user_id', () => {
    const wrapper = mount(NewSessionDialog, {
      props: { visible: true, defaultUserId: 'web-user' },
    })
    // Call handleCreate through component method access
    wrapper.vm.clientId = 'my-client'
    wrapper.vm.userId = 'my-user'
    wrapper.vm.handleCreate()
    expect(wrapper.emitted('create')).toBeTruthy()
    expect(wrapper.emitted('create')![0]).toEqual(['my-client', 'my-user'])
  })

  it('auto-generates client_id when empty', () => {
    const wrapper = mount(NewSessionDialog, {
      props: { visible: true, defaultUserId: 'web-user' },
    })
    // Leave clientId empty, set userId
    wrapper.vm.clientId = ''
    wrapper.vm.userId = 'test-user'
    wrapper.vm.handleCreate()
    expect(wrapper.emitted('create')).toBeTruthy()
    const [cid, uid] = wrapper.emitted('create')![0] as [string, string]
    expect(cid).toMatch(/^client_/)
    expect(uid).toBe('test-user')
  })

  it('falls back to defaultUserId when userId is empty', () => {
    const wrapper = mount(NewSessionDialog, {
      props: { visible: true, defaultUserId: 'fallback-user' },
    })
    wrapper.vm.clientId = 'c1'
    wrapper.vm.userId = ''
    wrapper.vm.handleCreate()
    expect(wrapper.emitted('create')).toBeTruthy()
    expect(wrapper.emitted('create')![0]).toEqual(['c1', 'fallback-user'])
  })

  it('emits close via handleClose', () => {
    const wrapper = mount(NewSessionDialog, {
      props: { visible: true },
    })
    wrapper.vm.handleClose()
    expect(wrapper.emitted('close')).toBeTruthy()
  })

  it('generates a short ID on open', () => {
    const wrapper = mount(NewSessionDialog, {
      props: { visible: true, defaultUserId: 'web-user' },
    })
    wrapper.vm.onOpen()
    expect(wrapper.vm.clientId).toMatch(/^client_/)
    expect(wrapper.vm.userId).toBe('web-user')
  })
})
