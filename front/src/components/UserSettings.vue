<template>
  <div>
    <button
      @click="isOpen = true"
      class="flex items-center text-sm font-medium px-3 py-2 rounded-full bg-blue-100 text-blue-700 hover:bg-blue-200 transition"
    >
      <span class="mr-2">{{ userStore.user?.username || '设置用户名' }}</span>
      <svg xmlns="http://www.w3.org/2000/svg" class="h-4 w-4" fill="none" viewBox="0 0 24 24" stroke="currentColor">
        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M15.232 5.232l3.536 3.536m-2.036-5.036a2.5 2.5 0 113.536 3.536L6.5 21.036H3v-3.572L16.732 3.732z" />
      </svg>
    </button>

    <!-- 弹窗 -->
    <div v-if="isOpen" class="fixed inset-0 flex items-center justify-center z-50">
      <div class="absolute inset-0 bg-black bg-opacity-30" @click="isOpen = false"></div>
      <div class="bg-white rounded-lg shadow-xl p-6 z-10 w-full max-w-md">
        <h3 class="text-lg font-bold text-gray-800 mb-4">设置用户信息</h3>

        <div class="mb-4">
          <label class="block text-sm font-medium text-gray-700 mb-1">
            用户名
          </label>
          <input
            v-model="username"
            type="text"
            class="w-full px-3 py-2 border border-gray-300 rounded-md shadow-sm focus:outline-none focus:ring-blue-500 focus:border-blue-500"
            placeholder="请输入您的用户名"
          >
        </div>

        <div class="flex justify-end space-x-3">
          <button
            @click="isOpen = false"
            class="px-4 py-2 text-sm font-medium text-gray-700 bg-gray-100 rounded-md hover:bg-gray-200"
          >
            取消
          </button>
          <button
            @click="saveSettings"
            class="px-4 py-2 text-sm font-medium text-white bg-blue-600 rounded-md hover:bg-blue-700"
          >
            保存
          </button>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { useUserStore } from '@/stores/user'

const userStore = useUserStore()
const isOpen = ref(false)
const username = ref('')

onMounted(() => {
  if (userStore.user) {
    username.value = userStore.user.username
  }
})

const saveSettings = () => {
  if (username.value.trim()) {
    userStore.updateUsername(username.value.trim())
    isOpen.value = false
  }
}
</script>
