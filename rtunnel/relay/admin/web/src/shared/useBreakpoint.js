import { computed, onBeforeUnmount, onMounted, ref } from 'vue'

// 简单断点：宽 <600 手机，<960 平板，其余桌面
export function useBreakpoint() {
  const width = ref(window.innerWidth)
  const onResize = () => { width.value = window.innerWidth }
  onMounted(() => window.addEventListener('resize', onResize))
  onBeforeUnmount(() => window.removeEventListener('resize', onResize))
  const isPhone = computed(() => width.value < 600)
  const isTablet = computed(() => width.value < 960)
  return { width, isPhone, isTablet }
}
