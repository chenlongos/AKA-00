import {Route, Routes} from "react-router-dom";
import BaseControlPage from "./pages/BaseControlPage.tsx";
import WiFiConfigPage from "./pages/WiFiConfigPage.tsx";

function App() {
    return (
        <Routes>
            <Route path="/" element={<BaseControlPage/>}/>
            <Route path="/wifi" element={<WiFiConfigPage/>}/>
        </Routes>
    )
}

export default App;
