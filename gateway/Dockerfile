FROM python:3.12-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY app ./app

ENV PYTHONUNBUFFERED=1
ENV GATEWAY_DB_PATH=/feeding/gateway.db
ENV GATEWAY_CONFIG_PATH=/feeding/config.json
ENV GATEWAY_TOKEN=
ENV GATEWAY_BIND_HOST=0.0.0.0
ENV GATEWAY_BIND_PORT=8080

EXPOSE 8080
VOLUME ["/feeding"]

CMD ["sh", "-c", "uvicorn app.main:app --host ${GATEWAY_BIND_HOST} --port ${GATEWAY_BIND_PORT}"]
