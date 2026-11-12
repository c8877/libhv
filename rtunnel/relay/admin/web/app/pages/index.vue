<script setup>
import { navigateTo } from '#app'

definePageMeta({
  middleware: () => navigateTo('/overview', { replace: true }),
})
</script>

<template>
  <div />
</template>
