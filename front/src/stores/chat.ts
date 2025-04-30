import { defineStore } from 'pinia'
import { ref } from 'vue'
import type { ChatMessage } from '@/types/chat'
import { useUserStore } from './user'

export const useChatStore = defineStore('chat', () => {
  const messages = ref<ChatMessage[]>([])
  const isConnected = ref(false)
  const pollingInterval = ref<number | null>(null)
  const lastTimestamp = ref<number>(0)
  const pollingDelay = 3000 // 轮询间隔，默认3秒

  // 初始化轮询
  const initializePolling = async () => {
    // 如果已经在轮询，先停止
    if (pollingInterval.value) {
      stopPolling()
    }

    // 先获取初始消息列表
    await fetchMessages()

    // 开始轮询
    isConnected.value = true
    pollingInterval.value = window.setInterval(fetchMessages, pollingDelay)
    console.log('轮询已启动')

    return true
  }

  // 获取消息
  const fetchMessages = async () => {
    try {
      const response = await fetch(`/api/chat/messages?since_timestamp=${lastTimestamp.value}`)

      if (!response.ok) {
        console.error('获取消息失败:', response.status)
        return
      }

      const data = await response.json()

      // 更新服务器时间
      if (data.server_time) {
        lastTimestamp.value = data.server_time
      }

      // 处理新消息
      if (data.has_new_messages && data.messages && data.messages.length > 0) {
        // 添加新消息到列表
        for (const message of data.messages) {
          addMessage(message)
        }
      }
    } catch (error) {
      console.error('轮询消息失败:', error)
    }
  }

  // 停止轮询
  const stopPolling = () => {
    if (pollingInterval.value) {
      clearInterval(pollingInterval.value)
      pollingInterval.value = null
      isConnected.value = false
      console.log('轮询已停止')
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

      if (response.ok) {
        // 发送成功后立即获取新消息，不等待下一次轮询
        await fetchMessages()
        return true
      }
      return false
    } catch (error) {
      console.error('发送消息失败:', error)
      return false
    }
  }

  return {
    messages,
    isConnected,
    initializePolling,
    stopPolling,
    sendMessage
  }
})