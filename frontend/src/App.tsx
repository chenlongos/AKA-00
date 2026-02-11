import {Route, Routes} from "react-router-dom";
import BaseControlPage from "./pages/BaseControlPage.tsx";
import SimPage from "./pages/SimPage.tsx";
import CarSimPage from "./pages/CarSimPage.tsx";

function App() {
    return (
        <Routes>
            <Route path="/" element={<BaseControlPage/>}/>
            <Route path="/sim" element={<SimPage/>}/>
            <Route path="/car" element={<CarSimPage/>}/>
        </Routes>
    )
}

export default App;
