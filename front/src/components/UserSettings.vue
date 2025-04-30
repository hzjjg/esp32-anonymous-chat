<template>
  <div>
    <button
      @click="isOpen = true"
      class="flex items-center text-sm font-medium px-4 py-2 rounded-full bg-white bg-opacity-20 hover:bg-opacity-30 transition-all shadow-sm backdrop-blur-sm"
    >
      <div class="flex-shrink-0 w-7 h-7 rounded-full bg-white bg-opacity-90 flex items-center justify-center text-blue-600 mr-2 shadow-sm">
        {{ getUserInitial() }}
      </div>
      <span class="hidden md:inline">{{ userStore.user?.username || '设置用户名' }}</span>
      <svg xmlns="http://www.w3.org/2000/svg" class="h-4 w-4 ml-2 opacity-70" fill="none" viewBox="0 0 24 24" stroke="currentColor">
        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M15.232 5.232l3.536 3.536m-2.036-5.036a2.5 2.5 0 113.536 3.536L6.5 21.036H3v-3.572L16.732 3.732z" />
      </svg>
    </button>

    <!-- 弹窗 -->
    <div v-if="isOpen" class="fixed inset-0 flex items-center justify-center z-50">
      <div class="absolute inset-0 bg-black bg-opacity-40 backdrop-blur-sm" @click="isOpen = false"></div>
      <div class="bg-white rounded-xl shadow-2xl p-6 z-10 w-full max-w-md transform transition-all duration-300 scale-in">
        <div class="flex justify-between items-center mb-6">
          <h3 class="text-xl font-bold text-gray-800 flex items-center">
            <svg xmlns="http://www.w3.org/2000/svg" class="h-6 w-6 mr-2 text-blue-600" fill="none" viewBox="0 0 24 24" stroke="currentColor">
              <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M16 7a4 4 0 11-8 0 4 4 0 018 0zM12 14a7 7 0 00-7 7h14a7 7 0 00-7-7z" />
            </svg>
            个人设置
          </h3>
          <button
            @click="isOpen = false"
            class="text-gray-400 hover:text-gray-600 p-1 rounded-full hover:bg-gray-100 transition-colors"
          >
            <svg xmlns="http://www.w3.org/2000/svg" class="h-5 w-5" fill="none" viewBox="0 0 24 24" stroke="currentColor">
              <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M6 18L18 6M6 6l12 12" />
            </svg>
          </button>
        </div>

        <div class="mb-6">
          <label class="block text-sm font-medium text-gray-700 mb-2">
            用户名
          </label>
          <input
            v-model="username"
            type="text"
            class="w-full px-4 py-3 border border-gray-300 rounded-xl shadow-sm focus:outline-none focus:ring-2 focus:ring-blue-500 focus:border-transparent transition-all text-gray-800"
            placeholder="请输入您的用户名"
            maxlength="20"
          >
          <div class="mt-1 text-xs text-gray-500 flex justify-between">
            <span>用于在聊天中显示</span>
            <span>{{ username.length }}/20</span>
          </div>
        </div>

        <div class="mt-8 flex justify-end space-x-3">
          <button
            @click="isOpen = false"
            class="px-5 py-2.5 text-sm font-medium text-gray-600 bg-gray-100 rounded-lg hover:bg-gray-200 transition-colors"
          >
            取消
          </button>
          <button
            @click="saveSettings"
            class="px-5 py-2.5 text-sm font-medium text-white bg-gradient-to-r from-blue-500 to-indigo-600 rounded-lg hover:from-blue-600 hover:to-indigo-700 transition-colors shadow-md disabled:opacity-50 disabled:cursor-not-allowed"
            :disabled="!username.trim() || username.trim().length < 2"
          >
            保存设置
          </button>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, computed } from 'vue'
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
  if (username.value.trim() && username.value.trim().length >= 2) {
    userStore.updateUsername(username.value.trim())
    isOpen.value = false
  }
}

// 获取用户名首字母或默认图标
const getUserInitial = () => {
  if (!userStore.user?.username) return '?'
  return userStore.user.username.charAt(0).toUpperCase()
}
</script>

<style scoped>
.scale-in {
  animation: scaleIn 0.2s ease-out forwards;
}

@keyframes scaleIn {
  from {
    opacity: 0;
    transform: scale(0.95);
  }
  to {
    opacity: 1;
    transform: scale(1);
  }
}
</style>
