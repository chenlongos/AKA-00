import {Route, Routes} from "react-router-dom";
import BaseControlPage from "./pages/BaseControlPage.tsx";
import WiFiConfigPage from "./pages/WiFiConfigPage.tsx";
import ArmAnglesPage from "./pages/ArmAnglesPage.tsx";
import DemoPage from "./pages/DemoPage.tsx";
import RCDemoPage from "./pages/RCDemoPage.tsx";
import GravityControlPage from "./pages/GravityControlPage.tsx";

function App() {
    return (
        <Routes>
            <Route path="/" element={<BaseControlPage/>}/>
            <Route path="/wifi" element={<WiFiConfigPage/>}/>
            <Route path="/arm-angles" element={<ArmAnglesPage/>}/>
            <Route path="/demo" element={<DemoPage/>}/>
            <Route path="/rc" element={<RCDemoPage/>}/>
            <Route path="/gravity" element={<GravityControlPage/>}/>
        </Routes>
    )
}

export default App;
