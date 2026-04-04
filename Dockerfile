FROM node:20-alpine

WORKDIR /app

# Зависимости отдельным слоем для кэширования
COPY package*.json ./
RUN npm ci --omit=dev

# Исходники приложения
COPY server/ ./server/
COPY public/ ./public/

EXPOSE 8080

ENV NODE_ENV=production \
    PORT=8080 \
    HOST=0.0.0.0

USER node

CMD ["node", "server/server.js"]
