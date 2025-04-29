import { defineStore } from 'pinia'
import { ref } from 'vue'
import type { ChatMessage } from '@/types/chat'
import { useUserStore } from './user'

export const useChatStore = defineStore('chat', () => {
  const messages = ref<ChatMessage[]>([])
  const isConnected = ref(false)
  const eventSource = ref<EventSource | null>(null)

  const initializeSSE = () => {
    if (eventSource.value) {
      eventSource.value.close()
    }

    const sse = new EventSource('/api/chat/events')
    eventSource.value = sse

    sse.addEventListener('open', () => {
      isConnected.value = true
      console.log('SSE连接已建立')
    })

    sse.addEventListener('error', (e) => {
      console.error('SSE连接错误:', e)
      isConnected.value = false

      // 尝试重新连接
      setTimeout(() => {
        if (sse.readyState === EventSource.CLOSED) {
          initializeSSE()
        }
      }, 3000)
    })

    sse.addEventListener('message', (e) => {
      try {
        const message: ChatMessage = JSON.parse(e.data)
        addMessage(message)
      } catch (error) {
        console.error('解析消息失败:', error)
      }
    })

    sse.addEventListener('messages', (e) => {
      try {
        const newMessages: ChatMessage[] = JSON.parse(e.data)
        messages.value = newMessages
      } catch (error) {
        console.error('解析消息列表失败:', error)
      }
    })

    sse.addEventListener('ping', () => {
      // 保持连接活跃的ping
    })

    return sse
  }

  const closeSSE = () => {
    if (eventSource.value) {
      eventSource.value.close()
      eventSource.value = null
      isConnected.value = false
    }
  }

  const addMessage = (message: ChatMessage) => {
    messages.value.push(message)

    // 限制消息数量为100条
    if (messages.value.length > 100) {
      messages.value.shift()
    }
  }

  const sendMessage = async (content: string) => {
    const userStore = useUserStore()
    if (!userStore.user) return false

    try {
      const response = await fetch('/api/chat/message', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json'
        },
        body: JSON.stringify({
          uuid: userStore.user.uuid,
          username: userStore.user.username,
          message: content
        })
      })

      return response.ok
    } catch (error) {
      console.error('发送消息失败:', error)
      return false
    }
  }

  return {
    messages,
    isConnected,
    initializeSSE,
    closeSSE,
    sendMessage
  }
})