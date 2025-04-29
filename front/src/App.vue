<template>
  <div v-if="!initialized" class="h-screen flex items-center justify-center">
    <div class="text-xl text-gray-600">加载中...</div>
  </div>

  <div v-else class="h-screen flex flex-col">
    <!-- 头部 -->
    <header class="bg-white shadow px-4 py-3 flex justify-between items-center">
      <h1 class="text-xl font-bold text-blue-600">匿名聊天室</h1>
      <div class="flex items-center space-x-3">
        <div class="text-sm text-gray-500">
          <span class="inline-block w-2 h-2 rounded-full"
            :class="chatStore.isConnected ? 'bg-green-500' : 'bg-red-500'"></span>
          {{ chatStore.isConnected ? '已连接' : '未连接' }}
        </div>
        <UserSettings />
      </div>
    </header>

    <!-- 聊天区域 -->
    <main class="flex-1 bg-gray-100 overflow-hidden flex flex-col">
      <div
        ref="messagesContainer"
        class="flex-1 overflow-y-auto p-4 flex flex-col"
        @scroll="handleScroll"
      >
        <template v-if="chatStore.messages.length > 0">
          <ChatMessage
            v-for="message in chatStore.messages"
            :key="`${message.uuid}-${message.timestamp}`"
            :message="message"
            class="flex flex-col"
          />
        </template>
        <div v-else class="flex-1 flex items-center justify-center text-gray-400">
          暂无消息，开始聊天吧
        </div>
      </div>

      <!-- 输入区域 -->
      <div class="bg-white border-t border-gray-200 p-3">
        <div class="flex items-end">
          <textarea
            v-model="messageInput"
            class="flex-1 bg-gray-50 border border-gray-300 rounded-lg px-3 py-2 resize-none focus:outline-none focus:ring-1 focus:ring-blue-500"
            :class="{ 'border-red-500': isOverLimit }"
            rows="2"
            placeholder="请输入消息..."
            @keydown.enter.prevent="sendMessage"
          ></textarea>
          <div class="ml-2 flex flex-col items-center">
            <EmojiPicker @select="insertEmoji" />
            <button
              @click="sendMessage"
              class="mt-2 px-4 py-2 bg-blue-600 text-white rounded-lg hover:bg-blue-700 transition"
              :disabled="!messageInput.trim() || isOverLimit"
              :class="{
                'opacity-50 cursor-not-allowed': !messageInput.trim() || isOverLimit
              }"
            >
              发送
            </button>
          </div>
        </div>
        <div
          class="text-xs mt-1 text-right"
          :class="isOverLimit ? 'text-red-500' : 'text-gray-500'"
        >
          {{ messageInput.length }}/150
        </div>
      </div>
    </main>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, onBeforeUnmount, nextTick, computed, watch } from 'vue'
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

  // 初始化聊天连接
  chatStore.initializeSSE()

  initialized.value = true

  // 如果有消息，滚动到底部
  nextTick(() => {
    scrollToBottom()
  })
})

onBeforeUnmount(() => {
  chatStore.closeSSE()
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