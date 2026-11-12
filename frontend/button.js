


const API_BASE = "";

document.getElementById("ontag").addEventListener("click", ()=>{
    fetch(`${API_BASE}/occupancy/button`);
});