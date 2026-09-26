# ===== BUILD STAGE =====
FROM node:20-alpine AS builder

WORKDIR /app

# Install dependencies
COPY package*.json ./
COPY prisma ./prisma/
RUN npm ci

# Generate Prisma Client & Build TypeScript
COPY . .
RUN DATABASE_URL="postgresql://placeholder:placeholder@localhost:5432/placeholder" npx prisma generate
RUN npm run build

# ===== PRODUCTION STAGE =====
FROM node:20-alpine

WORKDIR /app

# Copy production dependencies and built files
COPY package*.json ./
COPY --from=builder /app/node_modules ./node_modules

COPY --from=builder /app/dist ./dist
COPY --from=builder /app/node_modules/@prisma ./node_modules/@prisma
COPY --from=builder /app/prisma ./prisma
COPY --from=builder /app/prisma.config.ts ./

# Create start script that runs migrations before starting
RUN echo '#!/bin/sh' > /app/start.sh
RUN echo 'npx prisma db push' >> /app/start.sh
RUN echo 'node dist/server.js' >> /app/start.sh
RUN chmod +x /app/start.sh

EXPOSE 3000

CMD ["/app/start.sh"]
