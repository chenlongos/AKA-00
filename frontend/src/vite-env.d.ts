/// <reference types="vite/client" />

// 编译期开关：这台机器有没有板载屏 —— 与后端的 `AKA_WITH_SCREEN` 一一对应。
//   npm run build           → true（带屏版，产物进 static/）
//   npm run build:noscreen  → false（不带屏版，产物进 static-noscreen/）
// 用 `if (__WITH_SCREEN__)` 包住的代码，打包时会被直接删掉 —— 所以没有屏的机器
// 上，页面里连"屏幕显示"这个开关都不存在（而不是显示了再隐藏）。
declare const __WITH_SCREEN__: boolean;
