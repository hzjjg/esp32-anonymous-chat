<template>
  <div class="flex mb-5 max-w-[80%] mx-auto group" :class="[isCurrentUser ? 'flex-row-reverse' : 'flex-row']">
    <!-- 头像 -->
    <div
      class="flex-shrink-0 w-10 h-10 rounded-full flex items-center justify-center text-xl shadow-sm transform transition-transform group-hover:scale-105"
      :class="[
        isCurrentUser
          ? 'bg-gradient-to-br from-blue-400 to-indigo-500 ml-2'
          : 'bg-gradient-to-br from-gray-200 to-gray-300 mr-2'
      ]"
    >
      {{ getUserEmoji() }}
    </div>

    <!-- 消息内容区 -->
    <div class="flex flex-col max-w-[calc(100%-3rem)]" :class="[isCurrentUser ? 'items-end mr-2' : 'items-start ml-2']">
      <!-- 用户名和时间 -->
      <div class="flex items-center mb-1.5 text-xs" :class="[isCurrentUser ? 'flex-row-reverse' : 'flex-row']">
        <div class="font-medium" :class="isCurrentUser ? 'text-blue-600' : 'text-gray-700'">
          {{ message.username }}
        </div>
        <div class="mx-1 opacity-50">·</div>
        <div class="text-gray-400">{{ formatTime(message.timestamp) }}</div>
      </div>

      <!-- 消息气泡 -->
      <div
        class="p-3.5 rounded-2xl break-words whitespace-pre-wrap max-w-full message-bubble transition-shadow"
        :class="[
          isCurrentUser
            ? 'bg-gradient-to-r from-blue-500 to-indigo-600 text-white rounded-tr-none shadow-md'
            : 'bg-white text-gray-800 rounded-tl-none shadow-sm hover:shadow'
        ]"
      >
        {{ message.message }}
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import type { ChatMessage } from '@/types/chat'
import { useUserStore } from '@/stores/user'

const props = defineProps<{
  message: ChatMessage
}>()

const userStore = useUserStore()

const isCurrentUser = computed(() => {
  return userStore.user?.uuid === props.message.uuid
})

const formatTime = (timestamp: number) => {
  const date = new Date(timestamp * 1000)
  return date.toLocaleTimeString('zh-CN', {
    hour: '2-digit',
    minute: '2-digit'
  })
}

// 根据用户ID生成emoji头像
const getUserEmoji = () => {
  // 定义一组表情符号
  const emojis = [
    '😀', '😃', '😄', '😁', '😆', '😅', '🤣', '😂', '🙂', '🙃',
    '😉', '😊', '😇', '🥰', '😍', '🤩', '😘', '😗', '😚', '😙',
    '🐶', '🐱', '🐭', '🐹', '🐰', '🦊', '🐻', '🐼', '🐨', '🐯',
    '🦁', '🐮', '🐷', '🐸', '🐵', '🙈', '🙉', '🙊', '🐔', '🐧'
  ]

  // 使用用户UUID的最后几个字符来确定使用哪个emoji
  const uuidHash = props.message.uuid.split('-').pop() || ''
  const charSum = uuidHash.split('').reduce((sum, char) => sum + char.charCodeAt(0), 0)
  const emojiIndex = charSum % emojis.length

  return emojis[emojiIndex]
}
</script>

<style scoped>
.message-bubble {
  position: relative;
  transition: all 0.2s ease;
}

/* 添加消息气泡尖角 */
.message-bubble::before {
  content: '';
  position: absolute;
  width: 0;
  height: 0;
  border-style: solid;
  top: 8px;
}

/* 左侧消息气泡尖角 */
.rounded-tl-none::before {
  left: -8px;
  border-width: 0 8px 8px 0;
  border-color: transparent white transparent transparent;
}

/* 右侧消息气泡尖角 */
.rounded-tr-none::before {
  right: -8px;
  border-width: 8px 8px 0 0;
  border-color: #3b82f6 transparent transparent transparent;
}
</style>
