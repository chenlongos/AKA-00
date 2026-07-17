import {defineConfig} from 'vite'
import react from '@vitejs/plugin-react'
import path from 'path'
import fs from 'fs'

// https://vite.dev/config/
export default defineConfig({
    plugins: [
        react(),
        // 构建后同步到 ../static（给 Python Flask 使用）
        {
            name: 'copy-to-root-static',
            closeBundle() {
                const src = path.resolve(__dirname, '../dora/web-server/static')
                const dst = path.resolve(__dirname, '../static')
                if (fs.existsSync(dst)) fs.rmSync(dst, {recursive: true})
                fs.cpSync(src, dst, {recursive: true})
                console.log('  ✓ synced to ../static/')
            },
        },
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
        outDir: path.resolve(__dirname, '../dora/web-server/static'),
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
