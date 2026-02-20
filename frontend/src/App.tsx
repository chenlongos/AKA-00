import {useEffect} from "react";
import {Route, Routes} from "react-router-dom";
import BaseControlPage from "./pages/BaseControlPage.tsx";
import SimPage from "./pages/SimPage.tsx";

const allowSim = () => {
    const host = window.location.hostname;
    if (import.meta.env.DEV) return true;
    return host === "localhost" || host === "127.0.0.1";
};

const SimGuard = () => {
    useEffect(() => {
        if (!allowSim()) {
            window.location.replace("https://ai.maodouketang.cn/");
        }
    }, []);
    if (!allowSim()) return null;
    return <SimPage/>;
};

function App() {
    return (
        <Routes>
            <Route path="/" element={<SimGuard/>}/>
            <Route path="/control" element={<BaseControlPage/>}/>
            <Route path="/sim" element={<SimGuard/>}/>
        </Routes>
    )
}

export default App;
