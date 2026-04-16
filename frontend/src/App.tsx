import {Route, Routes} from "react-router-dom";
import BaseControlPage from "./pages/BaseControlPage.tsx";
import WiFiConfigPage from "./pages/WiFiConfigPage.tsx";
import ArmAnglesPage from "./pages/ArmAnglesPage.tsx";

function App() {
    return (
        <Routes>
            <Route path="/" element={<BaseControlPage/>}/>
            <Route path="/wifi" element={<WiFiConfigPage/>}/>
            <Route path="/arm-angles" element={<ArmAnglesPage/>}/>
        </Routes>
    )
}

export default App;
