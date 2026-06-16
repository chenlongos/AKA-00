import {useEffect, useState} from "react";
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

const TAB_PATHS = ["/", "/rc", "/demo", "/settings"];

function App() {
    const location = useLocation();
    const showTabBar = TAB_PATHS.includes(location.pathname);
    const [tabBarVisible, setTabBarVisible] = useState(true);

    // 监听 RCDemoPage 发出的 tab bar 显隐事件
    useEffect(() => {
        const handler = (e: Event) => {
            const {show} = (e as CustomEvent).detail;
            setTabBarVisible(show);
        };
        window.addEventListener("toggle-tabbar", handler);
        return () => window.removeEventListener("toggle-tabbar", handler);
    }, []);

    // 离开 /rc 时恢复 tab bar
    useEffect(() => {
        if (location.pathname !== "/rc") setTabBarVisible(true);
    }, [location.pathname]);

    return (
        <div style={{display: "flex", flexDirection: "column", height: "100dvh", width: "100%", maxWidth: "100vw", overflow: "hidden", background: "var(--color-bg)"}}>
            <div style={{flex: 1, overflowY: "auto", overflowX: "hidden", overscrollBehavior: "contain", WebkitOverflowScrolling: "touch"}}>
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
            {showTabBar && tabBarVisible && <TabBar />}
        </div>
    );
}

export default App;
