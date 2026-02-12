import {Route, Routes} from "react-router-dom";
import BaseControlPage from "./pages/BaseControlPage.tsx";
import SimPage from "./pages/SimPage.tsx";
import WebSocketTest from "./pages/WebSocketTest.tsx";

function App() {
    return (
        <Routes>
            <Route path="/" element={<BaseControlPage/>}/>
            <Route path="/sim" element={<SimPage/>}/>
            <Route path="/ws" element={<WebSocketTest/>}/>
        </Routes>
    )
}

export default App;
