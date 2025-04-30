<template>
  <div class="flex mb-4 max-w-[80%] mx-auto" :class="[isCurrentUser ? 'flex-row-reverse' : 'flex-row']">
    <!-- 头像 -->
    <div class="flex-shrink-0 w-10 h-10 rounded-full bg-gray-200 flex items-center justify-center text-xl">
      {{ getUserEmoji() }}
    </div>

    <!-- 消息内容区 -->
    <div class="flex flex-col mx-2" :class="[isCurrentUser ? 'items-end mr-2' : 'items-start ml-2']">
      <!-- 用户名和时间 -->
      <div class="flex items-center mb-1 text-xs text-gray-500" :class="[isCurrentUser ? 'flex-row-reverse' : 'flex-row']">
        <div class="font-medium">{{ message.username }}</div>
        <div class="mx-1">·</div>
        <div>{{ formatTime(message.timestamp) }}</div>
      </div>

      <!-- 消息气泡 -->
      <div
        class="p-3 rounded-lg break-words whitespace-pre-wrap max-w-[400px]"
        :class="[
          isCurrentUser ? 'bg-blue-500 text-white rounded-tr-none' : 'bg-white text-gray-800 rounded-tl-none shadow-sm'
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
