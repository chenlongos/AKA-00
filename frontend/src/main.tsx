import { createRoot } from 'react-dom/client'
import './index.css'
import App from './App.tsx'
import {BrowserRouter} from "react-router-dom";

// 带屏版打个标记。两用：
//   1) 打包脚本靠它在**不带屏包里**发现"你拿的是带屏版的 static/"（忘了重新 build）；
//   2) 浏览器控制台里 `window.__WITH_SCREEN__` 一眼看出这个页面是哪个版本的构建。
// 不带屏版整行会被打包器删掉（不在 bundle 里出现这个字符串）。
if (__WITH_SCREEN__) {
    (window as unknown as {__WITH_SCREEN__?: boolean}).__WITH_SCREEN__ = true;
}

createRoot(document.getElementById('root')!).render(
  <BrowserRouter>
    <App />
  </BrowserRouter>,
)
