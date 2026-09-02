import {defineConfig} from 'vite'
import react from '@vitejs/plugin-react'
import path from 'path'

// https://vite.dev/config/
export default defineConfig({
    plugins: [
        react(),
    ],
    server: {
        proxy: {
            "/api": {
                target: "http://localhost:5000",
                changeOrigin: true,
            },
            "/ws": {
                target: "ws://localhost:5000",
                ws: true,
                changeOrigin: true,
            },
        },
    },
    build: {
        // 构建产物直接输出到根 static/ —— capp（aka-capp）服务的就是这个目录
        // （板子上放 $AKA_HOME/static/，AKA_HOME=$HOME/AKA-00）
        outDir: path.resolve(__dirname, '../static'),
        emptyOutDir: true,
        rolldownOptions: {
            output: {
                entryFileNames: `assets/[name].js`,
                chunkFileNames: `assets/[name].js`,
                assetFileNames: `assets/[name].[ext]`
            }
        }
    }
})
