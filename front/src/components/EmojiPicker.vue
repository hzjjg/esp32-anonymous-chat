<template>
  <div class="relative">
    <button
      @click="togglePicker"
      class="p-2 rounded hover:bg-gray-200 transition"
      title="插入表情"
    >
      😊
    </button>

    <div
      v-if="isOpen"
      class="absolute bottom-10 right-0 bg-white rounded-lg shadow-lg p-2 grid grid-cols-6 gap-1 z-10"
      style="width: 200px;"
    >
      <button
        v-for="emoji in emojis"
        :key="emoji"
        @click="selectEmoji(emoji)"
        class="text-xl p-1 hover:bg-gray-100 rounded transition"
      >
        {{ emoji }}
      </button>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref } from 'vue'

const isOpen = ref(false)
const emojis = [
  '😀', '😃', '😄', '😁', '😆', '😅', '😂', '🤣', '😊', '😇',
  '🙂', '🙃', '😉', '😌', '😍', '🥰', '😘', '😗', '😙', '😚',
  '😋', '😛', '😝', '😜', '🤪', '🤨', '🧐', '🤓', '😎', '🤩',
  '👍', '👎', '👌', '✌️', '🤞', '🤟', '🤘', '👏', '🙌', '👋',
  '❤️', '🧡', '💛', '💚', '💙', '💜', '🖤', '💔', '💯', '💢'
]

const emit = defineEmits<{
  (e: 'select', emoji: string): void
}>()

const togglePicker = () => {
  isOpen.value = !isOpen.value
}

const selectEmoji = (emoji: string) => {
  emit('select', emoji)
  isOpen.value = false
}
</script>