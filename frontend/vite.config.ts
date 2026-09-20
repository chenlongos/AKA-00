import {defineConfig} from 'vite'
import react from '@vitejs/plugin-react'
import path from 'path'

// 板载屏编译开关 —— **与后端的 AKA_WITH_SCREEN 一一对应**（两边的"有没有屏"必须是同一个
// 决定，不能一边编掉、另一边还留着）：
//   npm run build            → 带屏版，产物进 static/
//   npm run build:noscreen   → 不带屏版，产物进 static-noscreen/
// 页面里用 `if (__WITH_SCREEN__)` 包住的代码会被打包器直接删掉（见 src/vite-env.d.ts）。
const withScreen = process.env.WITH_SCREEN !== "0";

// https://vite.dev/config/
export default defineConfig({
    plugins: [
        react(),
    ],
    define: {
        __WITH_SCREEN__: JSON.stringify(withScreen),
    },
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
        // 构建产物直接输出到仓库根的 static/（不带屏版是 static-noscreen/）——
        // capp（aka-capp）服务的就是这个目录（板子上放 $AKA_HOME/static/）。
        // 打包时按版本取对应的那份，见 cpp/Makefile 的 PACKAGE_RECIPE。
        outDir: path.resolve(__dirname, withScreen ? '../static' : '../static-noscreen'),
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
