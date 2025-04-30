import { defineStore } from 'pinia'
import { ref } from 'vue'
import type { ChatMessage } from '@/types/chat'
import { useUserStore } from './user'

export const useChatStore = defineStore('chat', () => {
  const messages = ref<ChatMessage[]>([])
  const isConnected = ref(false)
  const pollingTimeout = ref<number | null>(null)
  const lastTimestamp = ref<number>(0)
  const pollingDelay = 3000 // 轮询间隔，默认3秒
  const reconnectAttempts = ref(0)
  const maxReconnectAttempts = 5

  // 初始化轮询
  const initializePolling = async () => {
    // 如果已经在轮询，先停止
    if (pollingTimeout.value) {
      stopPolling()
    }

    // 重置重连计数
    reconnectAttempts.value = 0

    // 先获取初始消息列表
    const success = await fetchMessages()

    if (success) {
      // 开始轮询
      isConnected.value = true
      scheduleNextPoll()
      console.log('轮询已启动')
    }

    return success
  }

  // 调度下一次轮询
  const scheduleNextPoll = () => {
    pollingTimeout.value = window.setTimeout(async () => {
      await fetchMessages()
      // 只有在连接状态时才继续轮询
      if (isConnected.value) {
        scheduleNextPoll()
      }
    }, pollingDelay)
  }

  // 获取消息
  const fetchMessages = async () => {
    try {
      const response = await fetch(`/api/chat/messages?since_timestamp=${lastTimestamp.value}`)

      if (!response.ok) {
        console.error('获取消息失败:', response.status)
        handleConnectionError()
        return false
      }

      const data = await response.json()

      // 重置重连计数
      reconnectAttempts.value = 0

      // 确保连接状态为true
      if (!isConnected.value) {
        isConnected.value = true
      }

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

      return true
    } catch (error) {
      console.error('轮询消息失败:', error)
      handleConnectionError()
      return false
    }
  }

  // 处理连接错误
  const handleConnectionError = () => {
    isConnected.value = false
    reconnectAttempts.value++

    // 如果重连次数未超过最大值，尝试重连
    if (reconnectAttempts.value <= maxReconnectAttempts) {
      console.log(`连接失败，${pollingDelay}ms后重试 (${reconnectAttempts.value}/${maxReconnectAttempts})`)
      pollingTimeout.value = window.setTimeout(fetchMessages, pollingDelay)
    } else {
      console.error('达到最大重连次数，停止重连')
      stopPolling()
    }
  }

  // 停止轮询
  const stopPolling = () => {
    if (pollingTimeout.value) {
      clearTimeout(pollingTimeout.value)
      pollingTimeout.value = null
      isConnected.value = false
      console.log('轮询已停止')
    }
  }

  const addMessage = (message: ChatMessage) => {
    // 消息去重：检查是否已存在相同UUID和时间戳的消息
    const isDuplicate = messages.value.some(
      m => m.uuid === message.uuid && m.timestamp === message.timestamp
    )

    if (!isDuplicate) {
      messages.value.push(message)

      // 限制消息数量为100条
      if (messages.value.length > 100) {
        messages.value.shift()
      }
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