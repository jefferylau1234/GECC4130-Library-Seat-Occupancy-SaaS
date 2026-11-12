
const API_BASE = "";   
const present_occupancy = document.getElementById("present123");


let isLoadingPresent = false;

async function loadPresent(){

    if (isLoadingPresent) return;
    isLoadingPresent = true;              

    try{
    const [occupancyRes] = await Promise.all([
      fetch(`${API_BASE}/occupancy/present`)
    ]);

    if (!occupancyRes.ok) {
      throw new Error("API request failed");
    }

    const presentJSON = await occupancyRes.json();

    present_occupancy.textContent = `${presentJSON.o}`;

    } catch (error){
        console.error(error);
    } finally {
        isLoadingPresent = false;
  }
}


const timeID = setInterval(loadPresent, 1000);   // 10 seconds, 1 update