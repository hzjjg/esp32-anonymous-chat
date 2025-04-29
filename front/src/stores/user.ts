import { defineStore } from 'pinia'
import { ref } from 'vue'
import type { User } from '@/types/chat'

const STORAGE_KEY = 'anonymous_chat_user'

export const useUserStore = defineStore('user', () => {
  const user = ref<User | null>(null)

  const initialize = async () => {
    // 从localStorage加载用户数据
    const storedUser = localStorage.getItem(STORAGE_KEY)
    if (storedUser) {
      user.value = JSON.parse(storedUser)
      return user.value
    }

    // 如果没有用户数据，请求新的UUID
    try {
      const response = await fetch('/api/chat/uuid')
      if (!response.ok) throw new Error('获取UUID失败')

      const data = await response.json()
      const newUser: User = {
        uuid: data.uuid,
        username: `用户${data.uuid.substring(0, 4)}`
      }

      user.value = newUser
      localStorage.setItem(STORAGE_KEY, JSON.stringify(newUser))
      return newUser
    } catch (error) {
      console.error('初始化用户失败:', error)
      return null
    }
  }

  const updateUsername = (username: string) => {
    if (!user.value) return

    user.value.username = username
    localStorage.setItem(STORAGE_KEY, JSON.stringify(user.value))
  }

  return {
    user,
    initialize,
    updateUsername
  }
})