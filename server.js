const express = require("express");
const http = require("http");
const { Server } = require("socket.io");

const app = express();
const server = http.createServer(app);
const io = new Server(server);

app.use(express.static("public"));

let latestReading = null;

function isFiniteNumber(value) {
  return (
    typeof value === "number" &&
    Number.isFinite(value)
  );
}

io.on("connection", (socket) => {
  console.log("Connected:", socket.id);

  /*
   * Give newly opened dashboards the latest available reading.
   */
  if (latestReading) {
    socket.emit("noise-reading", latestReading);
  }

  socket.on("noise-reading", (reading) => {
    if (
      !reading ||
      !isFiniteNumber(reading.levelDbfs) ||
      !isFiniteNumber(reading.peakDbfs) ||
      !isFiniteNumber(reading.estimatedDbA) ||
      !isFiniteNumber(reading.calibrationOffset)
    ) {
      console.warn(
        "Rejected invalid reading from:",
        socket.id
      );

      return;
    }

    latestReading = {
      deviceId: String(
        reading.deviceId || "unknown"
      ).slice(0, 100),

      levelDbfs: Math.max(
        -100,
        Math.min(0, reading.levelDbfs)
      ),

      peakDbfs: Math.max(
        -100,
        Math.min(0, reading.peakDbfs)
      ),

      estimatedDbA: Math.max(
        0,
        Math.min(150, reading.estimatedDbA)
      ),

      calibrationOffset: Math.max(
        80,
        Math.min(140, reading.calibrationOffset)
      ),

      timestamp: new Date().toISOString()
    };

    /*
     * Send the reading to every connected browser,
     * including the sensor page.
     */
    io.emit("noise-reading", latestReading);
  });

  socket.on("disconnect", () => {
    console.log("Disconnected:", socket.id);
  });
});

const PORT = process.env.PORT || 3000;

server.listen(PORT, "0.0.0.0", () => {
  console.log(
    `Noise monitor running on port ${PORT}`
  );
});
