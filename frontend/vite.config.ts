import {defineConfig} from 'vite'
import react from '@vitejs/plugin-react'
import path from 'path'

// 板载屏编译开关 —— **与后端的 AKA_WITH_SCREEN 一一对应**（两边的"有没有屏"必须是同一个
// 决定，不能一边编掉、另一边还留着）：
//   npm run build            → 带屏版  ┐
//   npm run build:noscreen   → 不带屏版 ┘ 都产出到 static/，打包前 build 对版本
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
        // 构建产物**只有一个目录**：仓库根的 static/ —— capp（aka-capp）服务的就是它
        // （板子上放 $AKA_HOME/static/）。两个版本共用这一个目录，所以**打包前必须 build
        // 对版本**：先 npm run build[:noscreen]，再 make ota[: -noscreen]。
        // 拿错版本时打包脚本会报警（靠 main.tsx 里那个 __WITH_SCREEN__ 标记）。
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
