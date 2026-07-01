import { createRouter, createWebHistory, type RouteRecordRaw } from 'vue-router'
import MainLayout from '../layouts/MainLayout.vue'
import PackagesPage from '../pages/PackagesPage.vue'
import PackagePage from '../pages/PackagePage.vue'
import PackageCreator from '../components/PackageCreator.vue'
import ChatPage from '../pages/ChatPage.vue'
import SkillHubPage from '../pages/SkillHubPage.vue'
import SkillDetailPage from '../pages/SkillDetailPage.vue'

const routes: RouteRecordRaw[] = [
  {
    path: '/',
    component: MainLayout,
    redirect: '/packages',
    children: [
      {
        path: 'packages',
        name: 'packages',
        component: PackagesPage,
        meta: { title: '智能体包管理' },
      },
      {
        path: 'packages/create',
        name: 'package-create',
        component: PackageCreator,
        meta: { title: '创建智能体包' },
      },
      {
        path: 'packages/:name',
        name: 'package-detail',
        component: PackagePage,
        meta: { title: '包编辑器' },
      },
      {
        path: 'chat',
        name: 'chat',
        component: ChatPage,
        meta: { title: 'Agent 问答' },
      },
      {
        path: 'skill-hub',
        name: 'skill-hub',
        component: SkillHubPage,
        meta: { title: '技能中心' },
      },
      {
        path: 'skill-hub/:name',
        name: 'skill-hub-detail',
        component: SkillDetailPage,
        meta: { title: '技能详情' },
      },
    ],
  },
]

const router = createRouter({
  history: createWebHistory(),
  routes,
})

export default router
