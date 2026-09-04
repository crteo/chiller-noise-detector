const express = require("express");
const http = require("http");
const { Server } = require("socket.io");

const app = express();
const server = http.createServer(app);
const io = new Server(server);

app.use(express.static("public"));
app.use(
  express.json({
    limit: "10kb"
  })
);

let latestReading = null;
let latestPhoneReading = null;

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

  socket.on("phone-reading", (reading) => {
    if (
      !reading ||
      !isFiniteNumber(reading.levelDbfs) ||
      !isFiniteNumber(reading.peakDbfs) ||
      !isFiniteNumber(reading.estimatedDbA) ||
      !isFiniteNumber(reading.calibrationOffset)
    ) {
      console.warn("Invalid phone reading");
      return;
    }

    latestPhoneReading = {
      deviceId: String(
        reading.deviceId || "phone-microphone"
      ).slice(0, 100),

      levelDbfs: reading.levelDbfs,
      peakDbfs: reading.peakDbfs,
      estimatedDbA: reading.estimatedDbA,
      calibrationOffset: reading.calibrationOffset,
      timestamp: new Date().toISOString()
    };

    console.log(
      "Phone reading:",
      latestPhoneReading.estimatedDbA
    );
  });
  
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

app.get("/api/phone-reading", (request, response) => {
  if (!latestPhoneReading) {
    response.status(404).json({
      error: "No phone reading available"
    });

    return;
  }

  response.json(latestPhoneReading);
});


app.post("/api/esp32-reading", (request, response) => {
  const reading = request.body;

  if (
    !reading ||
    !isFiniteNumber(reading.levelDbfs) ||
    !isFiniteNumber(reading.peakDbfs) ||
    !isFiniteNumber(reading.estimatedDbA) ||
    !isFiniteNumber(reading.calibrationOffset)
  ) {
    response.status(400).json({
      error: "Invalid ESP32 reading"
    });

    return;
  }

  latestReading = {
    deviceId: "esp32-relay-01",
    sourceDeviceId: String(
      reading.deviceId || "phone-microphone"
    ).slice(0, 100),

    levelDbfs: reading.levelDbfs,
    peakDbfs: reading.peakDbfs,
    estimatedDbA: reading.estimatedDbA,
    calibrationOffset: reading.calibrationOffset,
    timestamp: new Date().toISOString()
  };

  io.emit("noise-reading", latestReading);

  response.status(202).json({
    accepted: true
  });
});

server.listen(PORT, "0.0.0.0", () => {
  console.log(
    `Noise monitor running on port ${PORT}`
  );
});
