import path from "path"
import tailwindcss from "@tailwindcss/vite"
//import viteCompression from "vite-plugin-compression"
import react from '@vitejs/plugin-react'
import { defineConfig } from 'vite'

// https://vite.dev/config/
export default defineConfig({
  base: './',
  plugins: [
    react(),
    tailwindcss(),
    /*
    // @ts-ignore
    viteCompression({
      algorithm: 'gzip',
      ext: '.gz',
      threshold: 0, // Força a compactação de TODOS os arquivos
      deleteOriginFile: true
    })
    */
  ],
  resolve: {
    alias: {
      "@": path.resolve(import.meta.dirname, "./src"),
    },
  },
  build: {
    outDir: "../data",
    // apaga os arquivos velhos da pasta
    emptyOutDir: true,
  }
})
