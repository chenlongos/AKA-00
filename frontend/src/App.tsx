import {Route, Routes} from "react-router-dom";
import BaseControlPage from "./pages/BaseControlPage.tsx";
import SimPage from "./pages/SimPage.tsx";

function App() {
    return (
        <Routes>
            <Route path="/" element={<BaseControlPage/>}/>
            <Route path="/sim" element={<SimPage/>}/>
        </Routes>
    )
}

export default App;
