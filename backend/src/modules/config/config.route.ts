import { FastifyInstance } from "fastify";
import { ZodTypeProvider } from "fastify-type-provider-zod";
import { z } from "zod";
import { validateEnv } from "../../config/env.js";
import { prisma } from "../../utils/prisma.js";
import { mqttPublish } from "../../services/mqtt.service.js";

const env = validateEnv();

export async function configRoutes(fastify: FastifyInstance) {
  const app = fastify.withTypeProvider<ZodTypeProvider>();

  // Device config
  // Device config
  app.get("/api/devices/:deviceId/config", {
    schema: { params: z.object({ deviceId: z.string() }) },
  }, async (request, reply) => {
    try {
      const { deviceId } = request.params;
      // 1. Try to find device-specific config
      let cfg = await prisma.systemConfig.findUnique({ where: { device_name: deviceId } });

      // 2. If not found, fall back to global config
      if (!cfg) {
        cfg = await prisma.systemConfig.findUnique({ where: { device_name: "__global__" } });
      }

      // 3. If still not found, create a global default
      if (!cfg) {
        cfg = await prisma.systemConfig.create({ 
          data: { 
            device_name: "__global__", 
            created_at: new Date(), 
            updated_at: new Date() 
          } 
        });
      }

      return cfg;
    } catch (err: any) {
      app.log.error(`Failed to fetch config: ${err.message}`);
      return reply.status(500).send({ error: "Failed to fetch configuration" });
    }
  });

  // Generic System config (Global Defaults)
  app.get("/api/config", async (request, reply) => {
    try {
      let cfg = await prisma.systemConfig.findUnique({ where: { device_name: "__global__" } });

      if (!cfg) {
        cfg = await prisma.systemConfig.create({ 
          data: { 
            device_name: "__global__", 
            created_at: new Date(), 
            updated_at: new Date() 
          } 
        });
      }

      return cfg;
    } catch (err: any) {
      app.log.error(`Failed to fetch system config: ${err.message}`);
      return reply.status(500).send({ error: "Failed to fetch system configuration" });
    }
  });

  // Post Config / Update
  app.post("/api/config", {
    schema: {
      body: z.object({
        deviceId: z.string().optional(),
        allDevices: z.boolean().optional(),
        deepSleepEnabled: z.boolean().optional(),
        sleepDurationSec: z.number().optional(),
        activeDurationMs: z.number().optional(),
        sensorReadInterval: z.number().optional(),
        mqttPublishInterval: z.number().optional(),
        pumpMinOnTime: z.number().optional(),
        pumpMaxOnTime: z.number().optional(),
        pumpCooldownTime: z.number().optional(),
        test_cmds: z.boolean().optional(),
        telegram_enabled: z.boolean().optional(),
        telegram_botToken: z.string().nullable().optional(),
        telegram_chatId: z.string().nullable().optional(),
        discord_enabled: z.boolean().optional(),
        discord_webhookUrl: z.string().nullable().optional(),
        transactionId: z.string().min(1),
      }),
    },
  }, async (request, reply) => {
    const { deviceId, allDevices, transactionId, ...payload } = request.body;
    const targetName = deviceId || "__global__";

    // 1. Persist to Database (Unique per device OR global)
    const up = await prisma.systemConfig.upsert({
      where: { device_name: targetName },
      create: { ...payload, device_name: targetName, updated_at: new Date() },
      update: { ...payload, updated_at: new Date() },
    });

    // 2. Build PARTIAL Firmware MQTT payload (Only include what changed)
    const firmwareConfigPayload: Record<string, any> = { transactionId };
    const mapping: Record<string, string> = {
      deepSleepEnabled: 'sleep_en',
      sleepDurationSec: 'sleep_sec',
      activeDurationMs: 'active_dur',
      sensorReadInterval: 'read_int',
      mqttPublishInterval: 'pub_int',
      pumpMaxOnTime: 'pump_max',
      pumpCooldownTime: 'pump_cool',
      test_cmds: 'test_cmds',
    };

    // Iterate over request body keys and map to firmware keys
    for (const [apiKey, firmwareKey] of Object.entries(mapping)) {
      if (apiKey in payload) {
        firmwareConfigPayload[firmwareKey] = (payload as any)[apiKey];
      }
    }

    const payloadStr = JSON.stringify(firmwareConfigPayload);

    // 3. TARGETED MQTT PUBLISHING
    // Skip publishing if the payload is empty (e.g. only deviceId was sent or unknown keys)
    if (Object.keys(firmwareConfigPayload).length === 0) {
      app.log.info('No firmware-relevant fields changed, skipping MQTT publish');
      return up;
    }

    if (deviceId) {
      // Scenario A: Only update and notify the specific device
      await mqttPublish(`${env.MQTT_BASE_TOPIC}/${deviceId}/cmd/config`, payloadStr);
      app.log.info({ deviceId }, 'Config updated and published to specific device');
    } 
    else if (allDevices === true) {
      // Scenario B: Professional Broadcast (Fleet-wide update)
      const devices = await prisma.device.findMany({ select: { device_id: true } });
      app.log.info({ count: devices.length }, 'Broadcasting config to all devices iteratively');
      
      for (const d of devices) {
        await mqttPublish(`${env.MQTT_BASE_TOPIC}/${d.device_id}/cmd/config`, payloadStr).catch(err => {
          app.log.error({ err, deviceId: d.device_id }, 'Failed to publish config to device');
        });
      }
    }
    // Scenario C: Neither -> Only DB updated (Useful for setting global defaults without waking devices)

    return up;
  });

}
