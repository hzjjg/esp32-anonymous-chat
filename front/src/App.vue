<template>
  <div v-if="!initialized" class="h-screen flex items-center justify-center bg-gradient-to-r from-blue-50 to-indigo-50">
    <div class="text-2xl text-blue-600 font-medium flex flex-col items-center">
      <svg class="animate-spin -ml-1 mr-3 h-8 w-8 text-blue-500 mb-3" xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24">
        <circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"></circle>
        <path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z"></path>
      </svg>
      加载中...
    </div>
  </div>

  <div v-else class="h-screen flex flex-col bg-gray-50">
    <!-- 头部 -->
    <header class="bg-gradient-to-r from-blue-600 to-indigo-600 text-white px-6 py-4 shadow-md">
      <div class="max-w-6xl mx-auto flex justify-between items-center">
        <div class="flex items-center">
          <svg xmlns="http://www.w3.org/2000/svg" class="h-7 w-7 mr-3" fill="none" viewBox="0 0 24 24" stroke="currentColor">
            <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M17 8h2a2 2 0 012 2v6a2 2 0 01-2 2h-2v4l-4-4H9a1.994 1.994 0 01-1.414-.586m0 0L11 14h4a2 2 0 002-2V6a2 2 0 00-2-2H5a2 2 0 00-2 2v6a2 2 0 002 2h2v4l.586-.586z" />
          </svg>
          <h1 class="text-xl font-bold">Anonymous Chat Room</h1>
        </div>
        <div class="flex items-center space-x-4">
          <div class="flex items-center px-3 py-1.5 bg-white bg-opacity-20 rounded-full text-sm">
            <span class="inline-block w-2.5 h-2.5 rounded-full mr-2"
              :class="chatStore.isConnected ? 'bg-green-400' : 'bg-red-400'"></span>
            {{ chatStore.isConnected ? '已连接' : '未连接' }}
          </div>
          <UserSettings />
        </div>
      </div>
    </header>

    <!-- 聊天区域 -->
    <main class="flex-1 bg-gradient-to-b from-gray-50 to-gray-100 overflow-hidden flex flex-col">
      <div
        ref="messagesContainer"
        class="flex-1 overflow-y-auto p-4 flex flex-col mx-auto w-full max-w-4xl"
        @scroll="handleScroll"
      >
        <template v-if="chatStore.messages.length > 0">
          <ChatMessage
            v-for="message in chatStore.messages"
            :key="`${message.uuid}-${message.timestamp}`"
            :message="message"
          />
        </template>
        <div v-else class="flex-1 flex flex-col items-center justify-center text-gray-400">
          <svg xmlns="http://www.w3.org/2000/svg" class="h-16 w-16 mb-4 text-gray-300" fill="none" viewBox="0 0 24 24" stroke="currentColor">
            <path stroke-linecap="round" stroke-linejoin="round" stroke-width="1" d="M8 12h.01M12 12h.01M16 12h.01M21 12c0 4.418-4.03 8-9 8a9.863 9.863 0 01-4.255-.949L3 20l1.395-3.72C3.512 15.042 3 13.574 3 12c0-4.418 4.03-8 9-8s9 3.582 9 8z" />
          </svg>
          暂无消息，开始聊天吧
        </div>
      </div>

      <!-- 输入区域 -->
      <div class="bg-white border-t border-gray-200 shadow-md">
        <div class="max-w-4xl mx-auto p-4">
          <div class="flex items-center">
            <div class="flex-1 relative">
              <textarea
                v-model="messageInput"
                class="w-full bg-gray-50 border border-gray-300 rounded-2xl px-4 py-3 resize-none focus:outline-none focus:ring-2 focus:ring-blue-500 focus:border-transparent shadow-sm"
                :class="{ 'border-red-500 focus:ring-red-500': isOverLimit }"
                rows="3"
                placeholder="请输入消息..."
                @keydown.enter.prevent="sendMessage"
              ></textarea>
              <div
                class="absolute bottom-2 right-3 text-xs"
                :class="isOverLimit ? 'text-red-500' : 'text-gray-400'"
              >
                {{ messageInput.length }}/150
              </div>
            </div>
            <div class="ml-3 flex flex-col items-center">
              <EmojiPicker @select="insertEmoji" />
              <button
                @click="sendMessage"
                class="mt-2 px-5 py-3 bg-gradient-to-r from-blue-500 to-indigo-600 text-white rounded-full hover:from-blue-600 hover:to-indigo-700 transition shadow-md flex items-center justify-center"
                :disabled="!messageInput.trim() || isOverLimit"
                :class="{
                  'opacity-50 cursor-not-allowed': !messageInput.trim() || isOverLimit
                }"
              >
                <svg xmlns="http://www.w3.org/2000/svg" class="h-5 w-5" fill="none" viewBox="0 0 24 24" stroke="currentColor">
                  <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 19l9 2-9-18-9 18 9-2zm0 0v-8" />
                </svg>
              </button>
            </div>
          </div>
        </div>
      </div>
    </main>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, nextTick, computed, watch } from 'vue'
import ChatMessage from '@/components/ChatMessage.vue'
import EmojiPicker from '@/components/EmojiPicker.vue'
import UserSettings from '@/components/UserSettings.vue'
import { useUserStore } from '@/stores/user'
import { useChatStore } from '@/stores/chat'

const userStore = useUserStore()
const chatStore = useChatStore()

const initialized = ref(false)
const messageInput = ref('')
const messagesContainer = ref<HTMLElement | null>(null)
const shouldAutoScroll = ref(true)

const isOverLimit = computed(() => messageInput.value.length > 150)

onMounted(async () => {
  // 初始化用户
  await userStore.initialize()

  // 初始化聊天
  await chatStore.initializePolling()

  initialized.value = true

  // 如果有消息，滚动到底部
  nextTick(() => {
    scrollToBottom()
  })
})

// 监听消息变化，如果应该自动滚动，则滚动到底部
watch(() => chatStore.messages.length, () => {
  if (shouldAutoScroll.value) {
    nextTick(() => {
      scrollToBottom()
    })
  }
})

// 处理滚动事件，判断是否应该自动滚动
const handleScroll = () => {
  if (!messagesContainer.value) return

  const { scrollTop, scrollHeight, clientHeight } = messagesContainer.value
  const isNearBottom = scrollHeight - scrollTop - clientHeight < 100

  shouldAutoScroll.value = isNearBottom
}

// 滚动到底部
const scrollToBottom = () => {
  if (!messagesContainer.value) return

  messagesContainer.value.scrollTop = messagesContainer.value.scrollHeight
}

// 插入表情
const insertEmoji = (emoji: string) => {
  messageInput.value += emoji
}

// 发送消息
const sendMessage = async () => {
  const content = messageInput.value.trim()
  if (!content || isOverLimit.value) return

  const success = await chatStore.sendMessage(content)
  if (success) {
    messageInput.value = ''
  }
}
</script>