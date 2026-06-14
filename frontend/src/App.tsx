import {Route, Routes, useLocation} from "react-router-dom";
import TabBar from "./components/TabBar.tsx";
import BaseControlPage from "./pages/BaseControlPage.tsx";
import WiFiConfigPage from "./pages/WiFiConfigPage.tsx";
import ArmAnglesPage from "./pages/ArmAnglesPage.tsx";
import DemoPage from "./pages/DemoPage.tsx";
import RCDemoPage from "./pages/RCDemoPage.tsx";
import GravityControlPage from "./pages/GravityControlPage.tsx";
import SettingsPage from "./pages/SettingsPage.tsx";
import OTAPage from "./pages/OTAPage.tsx";

// 带 TabBar 的页面路径
const TAB_PATHS = ["/", "/rc", "/demo", "/settings"];

function App() {
    const location = useLocation();
    const showTabBar = TAB_PATHS.includes(location.pathname);

    return (
        <div
            style={{
                display: "flex",
                flexDirection: "column",
                height: "100dvh",
                width: "100%",
                maxWidth: "100vw",
                overflow: "hidden",
                background: "var(--color-bg)",
            }}
        >
            {/* 内容区：flex-1 填充剩余空间，可滚动 */}
            <div
                style={{
                    flex: 1,
                    overflowY: "auto",
                    overflowX: "hidden",
                    overscrollBehavior: "contain",
                    WebkitOverflowScrolling: "touch",
                }}
            >
                <Routes>
                    <Route path="/" element={<BaseControlPage />} />
                    <Route path="/rc" element={<RCDemoPage />} />
                    <Route path="/demo" element={<DemoPage />} />
                    <Route path="/settings" element={<SettingsPage />} />
                    <Route path="/wifi" element={<WiFiConfigPage />} />
                    <Route path="/arm-angles" element={<ArmAnglesPage />} />
                    <Route path="/ota" element={<OTAPage />} />
                    <Route path="/gravity" element={<GravityControlPage />} />
                </Routes>
            </div>

            {/* 底部 TabBar */}
            {showTabBar && <TabBar />}
        </div>
    );
}

export default App;
