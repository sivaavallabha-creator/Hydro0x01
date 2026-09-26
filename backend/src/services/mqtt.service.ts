import mqtt, { MqttClient } from 'mqtt'
import { validateEnv } from '../config/env.js'
import { createLogger } from '../utils/logger.js'
import { writeTelemetry } from './telemetry.service.js'
import { broadcastTelemetry } from '../sockets/socket.js'
import { updateDeviceStatus, updateDevicePower, updateDeviceHeartbeat } from './device-status.service.js'
import { setSensorStatus } from './sensor-status-cache.js'
import { notifyStatusWaiters } from './mqtt-command-waiter.js'
import { prisma } from '../utils/prisma.js'
import { sendNotification } from './notification.service.js'

const env = validateEnv()
const logger = createLogger()

let client: MqttClient | null = null

/** 
 * REFACTORED MQTT SERVICE - VERSION 2.0
 * This version uses MQTT v5 and keep-alive to prevent cloud idle timeouts.
 */

async function ensureMqttConnected(timeoutMs = 7000): Promise<void> {
  if (!client) {
    logger.info('Initializing MQTT connection from ensureMqttConnected...')
    await initMqtt()
  }

  const c = client!
  if (c.connected) return

  logger.warn(`MQTT not connected, waiting up to ${timeoutMs}ms... (Tuned v2.1)`)

  await new Promise<void>((resolve, reject) => {
    let settled = false
    const to = setTimeout(() => {
      if (settled) return
      settled = true
      c.removeListener('connect', onOk)
      reject(new Error('MQTT broker is offline. Command aborted to prevent API hang (Refactored v2.1).'))
    }, timeoutMs)

    const onOk = () => {
      if (settled) return
      settled = true
      clearTimeout(to)
      resolve()
    }
    c.prependOnceListener('connect', onOk)
    if (c.connected) onOk()
  })
}

let messageCount = 0

export async function initMqtt() {
  if (client) return client

  const url = `mqtts://${env.MQTT_BROKER}:${env.MQTT_PORT}`
  // A persistent MQTT session must reconnect with the same client identity. A random value
  // made clean:false and sessionExpiryInterval ineffective and made publisher attribution
  // impossible to bind into deployment approval.
  const clientId = env.MQTT_CLIENT_ID ?? 'hydro-api'

  logger.info({ url, clientId }, 'REFACTORED MQTT SERVICE VERSION 2.1 - STARTING')

  client = mqtt.connect(url, {
    username: env.MQTT_USER,
    password: env.MQTT_PASSWORD,
    rejectUnauthorized: false,
    protocolVersion: 5,       // Enforce MQTT 5
    keepalive: 30,            // Keep TCP alive
    reconnectPeriod: 5000,    // Tune: Give the broker more breathing room (5s)
    connectTimeout: 15000,    // Tune: Increase for slow TLS handshake (15s)
    clientId: clientId,
    clean: false,             // Tune: Persistence (requires sessionExpiryInterval)
    properties: {
      sessionExpiryInterval: 3600 // Keep session for 1 hour on broker
    }
  })

  client.on('connect', (connack) => {
    logger.info({ sessionPresent: connack.sessionPresent }, 'Connected to MQTT broker (v2.1)')
    // Subscribe to sensor and power topics
    client?.subscribe(`${env.MQTT_BASE_TOPIC}/+/sensors/#`, { qos: 1 }, (err: Error | null) => {
      if (err) logger.error({ err }, 'Failed to subscribe to sensors')
      else logger.info('Subscribed to sensor topics')
    })
    client?.subscribe(`${env.MQTT_BASE_TOPIC}/+/power/#`, { qos: 1 }, (err: Error | null) => {
      if (err) logger.error({ err }, 'Failed to subscribe to power')
      else logger.info('Subscribed to power topics')
    })
    // Subscribe to status and heartbeat topics for device lifecycle tracking
    client?.subscribe(`${env.MQTT_BASE_TOPIC}/+/status`, { qos: 1 }, (err: Error | null) => {
      if (err) logger.error({ err }, 'Failed to subscribe to status')
      else logger.info('Subscribed to status topics')
    })
    client?.subscribe(`${env.MQTT_BASE_TOPIC}/+/heartbeat`, { qos: 1 }, (err: Error | null) => {
      if (err) logger.error({ err }, 'Failed to subscribe to heartbeat')
      else logger.info('Subscribed to heartbeat topics')
    })
    // DIAGNOSTICS & ERRORS
    client?.subscribe(`${env.MQTT_BASE_TOPIC}/+/errors`, { qos: 1 }, (err: Error | null) => {
      if (err) logger.error({ err }, 'Failed to subscribe to errors')
      else logger.info('Subscribed to error topics')
    })
    client?.subscribe(`${env.MQTT_BASE_TOPIC}/+/diagnostics`, { qos: 1 }, (err: Error | null) => {
      if (err) logger.error({ err }, 'Failed to subscribe to diagnostics')
      else logger.info('Subscribed to diagnostics topics')
    })
  })

  client.on('message', async (topic: string, payload: Buffer) => {
    messageCount++
    if (messageCount % 100 === 0) {
      logger.info({ messageCount }, 'MQTT throughput check (100 messages processed)')
    }
    try {
      const payloadStr = payload.toString()

      // Split topic into parts
      // Example: OpenHydroponic/HydroNode_01/sensors/water/temperature
      // Parts: [OpenHydroponic, HydroNode_01, sensors, water, temperature]
      const topicParts = topic.split('/')

      // Validate minimum topic structure
      if (topicParts.length < 3 || topicParts[0] !== env.MQTT_BASE_TOPIC) {
        logger.debug({ topic }, 'Invalid topic structure')
        return
      }

      // Extract device ID
      const deviceId = topicParts[1]
      if (!deviceId) {
        logger.warn({ topic }, 'No device ID in topic')
        return
      }

      // Handle Errors and Diagnostics
      if (topic.endsWith('/errors') || topic.endsWith('/diagnostics')) {
        const type = topic.endsWith('/errors') ? 'error' : 'diagnostic';
        
        // NOISE FILTER: Skip persisting "Test commands disabled" as a permanent alert
        const isNoise = payloadStr.includes("Test commands disabled");
        
        if (!isNoise) {
          logger.warn({ deviceId, type, payload: payloadStr }, `System alert received: ${payloadStr}`);
          await prisma.systemAlert.create({
            data: {
              device_id: deviceId,
              type: type,
              message: payloadStr.substring(0, 1000) // Ensure it fits in the DB
            }
          }).catch((err: any) => logger.error({ err }, 'Failed to save system alert'));

          // Fire off external push notifications natively
          if (type === 'error') {
            sendNotification({
              level: 'critical',
              title: `System Alert: ${deviceId}`,
              message: payloadStr,
              device_id: deviceId
            });
          }
        } else {
          logger.debug({ deviceId, payload: payloadStr }, 'Filtered noise alert (not saved to DB)');
        }

        broadcastTelemetry({
          deviceId,
          type: `alert_${type}`,
          data: { message: payloadStr },
          timestamp: new Date().toISOString(),
        });
        return;
      }

      // Aggregated sensor health JSON (not a numeric leaf)
      if (topic.endsWith('/sensors/status')) {
        try {
          const j = JSON.parse(payloadStr) as Record<string, unknown>
          setSensorStatus(deviceId, j)
          broadcastTelemetry({
            deviceId,
            type: 'sensor_status',
            data: j,
            timestamp: new Date().toISOString(),
          })
        } catch (parseErr) {
          logger.warn({ deviceId, payload: payloadStr }, 'Failed to parse sensors/status')
        }
        return
      }

      // Handle status and heartbeat messages (device lifecycle)
      if (topic.includes('/status') && !topic.includes('/sensors/status')) {
        try {
          const statusData = JSON.parse(payloadStr) as Record<string, unknown>
          notifyStatusWaiters(deviceId, statusData)
          await updateDeviceStatus(deviceId, statusData)
          broadcastTelemetry({
            deviceId,
            type: 'status',
            data: statusData,
            timestamp: new Date().toISOString(),
          })
        } catch (parseErr) {
          logger.warn({ deviceId, payload: payloadStr }, 'Failed to parse status message')
        }
        return
      }

      if (topic.includes('/heartbeat')) {
        try {
          const heartbeatData = JSON.parse(payloadStr)
          logger.debug({ deviceId, uptime: heartbeatData.uptime }, 'Device heartbeat')
          // Persist heartbeat to DB for later retrieval
          await updateDeviceHeartbeat(deviceId, heartbeatData as any)
          broadcastTelemetry({
            deviceId,
            type: 'heartbeat',
            data: heartbeatData,
            timestamp: new Date().toISOString(),
          })
        } catch (parseErr) {
          logger.debug({ deviceId, payload: payloadStr }, 'Failed to parse heartbeat')
        }
        return
      }

      // Parse numeric value from payload
      let value: number
      try {
        // Try parsing as JSON first (in case payload is {"value": 22.5})
        const parsed = JSON.parse(payloadStr)
        value = typeof parsed === 'number' ? parsed : parsed.value
      } catch {
        // Fall back to direct numeric parsing
        value = parseFloat(payloadStr)
      }

      // Validate numeric value
      if (!Number.isFinite(value)) {
        logger.debug({ deviceId, topic, payload: payloadStr }, 'Invalid numeric value')
        return
      }

      // Construct sensor tag from topic parts
      // Example: sensors/water/temperature -> water_temperature
      // Example: power/battery -> power_battery
      const section = topicParts[2]

      let sensor: string | null = null

      // sensors/water/temperature
      if (section === 'sensors' && topicParts.length >= 5) {
        const category = topicParts[3]
        const name = topicParts[4]
        sensor = `${category}_${name}`
      }

      // power/battery
      else if (section === 'power' && topicParts.length >= 4) {
        const name = topicParts[3]
        sensor = `power_${name}`
      }

      if (!sensor) {
        logger.warn({ topic }, 'Unsupported telemetry topic')
        return
      }


      logger.debug(
        { deviceId, topic, sensor, value },
        'Storing telemetry'
      )

      // Write to InfluxDB
      //await writeTelemetry(deviceId, sensorCategory, sensorName, value)
      await writeTelemetry(deviceId, sensor, value)

      // If this was a power topic, also persist last_power in the device record
      try {
        if (sensor.startsWith('power_')) {
          // store the power payload as last_power
          await updateDevicePower(deviceId, { [sensor]: value })
        }
      } catch (e) {
        logger.debug({ err: e }, 'Failed to update device power')
      }

      // Broadcast to WebSocket clients
      broadcastTelemetry({
        deviceId,
        sensor,
        value,
        topic,
        timestamp: new Date().toISOString(),
      })
    } catch (err) {
      logger.error({ err, topic: topic }, 'Failed processing MQTT message')
    }
  })

  client.on('error', (err: Error) => logger.error({ err }, 'MQTT error (Refactored v2.1)'))
  client.on('disconnect', (packet: any) => {
    logger.warn({ reasonCode: packet?.reasonCode, properties: packet?.properties }, 'MQTT Disconnected from Broker (v2.1)')
  })
  client.on('offline', () => logger.warn('MQTT client went offline (Refactored v2.1)'))
  client.on('close', () => logger.info('MQTT connection closed (Refactored v2.1)'))
  client.on('reconnect', () => logger.info('MQTT client attempting to reconnect... (Refactored v2.1)'))

  return client
}

export async function mqttPublish(topic: string, payload: string) {
  // Safety Guard: MQTT v5 does not allow publishing to wildcard topics (+, #)
  if (topic.includes('+') || topic.includes('#')) {
    const err = new Error(`Illegal MQTT Publish: Topic "${topic}" contains wildcards (+/#). Discarding to prevent broker disconnect.`);
    logger.error({ topic, err: err.message }, 'MQTT safety guard triggered');
    throw err;
  }

  await ensureMqttConnected()
  const c = client!
  return new Promise<void>((resolve, reject) => {
    c.publish(topic, payload, { qos: 1 }, (err?: Error) => {
      if (err) {
        logger.error({ err, topic }, 'MQTT publish failed')
        return reject(err)
      }
      logger.info({ topic }, 'Published MQTT message')
      resolve()
    })
  })
}
