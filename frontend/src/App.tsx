import {Route, Routes} from "react-router-dom";
import BaseControlPage from "./pages/BaseControlPage.tsx";

function App() {
    return (
        <Routes>
            <Route path="/" element={<BaseControlPage/>}/>
        </Routes>
    )
}

export default App;
