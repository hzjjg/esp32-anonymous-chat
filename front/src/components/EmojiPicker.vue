<template>
  <div class="relative">
    <button
      @click.stop="togglePicker"
      class="p-2.5 rounded-full bg-gradient-to-r from-blue-50 to-indigo-50 hover:from-blue-100 hover:to-indigo-100 transition-all shadow-sm transform hover:scale-105 active:scale-95 text-xl"
      title="插入表情"
    >
      <span class="flex items-center justify-center w-6 h-6">😊</span>
    </button>

    <div
      v-if="isOpen"
      ref="emojiPanel"
      class="absolute bottom-14 right-0 bg-white rounded-xl shadow-xl p-3 z-10 border border-gray-100 emoji-panel"
      style="width: 280px;"
      @click.stop
    >
      <div class="mb-2 pb-2 border-b border-gray-100 flex justify-between items-center">
        <span class="text-xs font-medium text-gray-500">选择表情</span>
        <button
          @click="isOpen = false"
          class="text-gray-400 hover:text-gray-600 p-1 rounded-full hover:bg-gray-100"
        >
          <svg xmlns="http://www.w3.org/2000/svg" class="h-4 w-4" fill="none" viewBox="0 0 24 24" stroke="currentColor">
            <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M6 18L18 6M6 6l12 12" />
          </svg>
        </button>
      </div>

      <div class="grid grid-cols-7 gap-1">
        <button
          v-for="emoji in emojis"
          :key="emoji"
          @click="selectEmoji(emoji)"
          class="text-xl p-2 hover:bg-gray-100 rounded-lg transition-all hover:scale-110 active:scale-95"
        >
          {{ emoji }}
        </button>
      </div>

      <div class="mt-2 pt-2 border-t border-gray-100 text-center">
        <span class="text-xs text-gray-400">常用表情</span>
        <div class="mt-1 flex justify-center space-x-2">
          <button
            v-for="emoji in frequentEmojis"
            :key="emoji"
            @click="selectEmoji(emoji)"
            class="text-xl p-1 hover:bg-blue-50 rounded-lg transition-all hover:scale-110"
          >
            {{ emoji }}
          </button>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, onUnmounted } from 'vue'

const isOpen = ref(false)
const emojiPanel = ref<HTMLElement | null>(null)

const emojis = [
  '😀', '😃', '😄', '😁', '😆', '😅', '😂', '🤣', '😊', '😇',
  '🙂', '🙃', '😉', '😌', '😍', '🥰', '😘', '😗', '😙', '😚',
  '😋', '😛', '😝', '😜', '🤪', '🤨', '🧐', '🤓', '😎', '🤩',
  '👍', '👎', '👌', '✌️', '🤞', '🤟', '🤘', '👏', '🙌', '👋',
  '❤️', '🧡', '💛', '💚', '💙', '💜', '🖤', '💔', '💯', '💢'
]

const frequentEmojis = ['👍', '❤️', '😊', '🙏', '😂']

const emit = defineEmits<{
  (e: 'select', emoji: string): void
}>()

const togglePicker = (event: Event) => {
  event.stopPropagation()
  isOpen.value = !isOpen.value
}

const selectEmoji = (emoji: string) => {
  emit('select', emoji)
  isOpen.value = false
}

// 点击外部关闭表情面板
const handleClickOutside = (event: MouseEvent) => {
  if (isOpen.value) {
    isOpen.value = false
  }
}

onMounted(() => {
  setTimeout(() => {
    document.addEventListener('click', handleClickOutside)
  }, 0)
})

onUnmounted(() => {
  document.removeEventListener('click', handleClickOutside)
})
</script>

<style scoped>
.emoji-panel {
  animation: fadeIn 0.2s ease-out;
}

@keyframes fadeIn {
  from {
    opacity: 0;
    transform: translateY(10px);
  }
  to {
    opacity: 1;
    transform: translateY(0);
  }
}
</style>