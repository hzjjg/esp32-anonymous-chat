<template>
  <div
    :class="[
      'p-3 mb-2 rounded-lg max-w-[85%]',
      isCurrentUser ? 'bg-blue-500 text-white self-end' : 'bg-white text-gray-800 self-start',
    ]"
  >
    <div class="text-sm opacity-75 mb-1" :class="{ 'text-right': isCurrentUser }">
      {{ message.username }}
    </div>
    <div class="break-words whitespace-pre-wrap">{{ message.message }}</div>
    <div class="text-xs opacity-60 mt-1" :class="{ 'text-right': isCurrentUser }">
      {{ formatTime(message.timestamp) }}
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
</script>
