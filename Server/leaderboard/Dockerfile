FROM python:3.12-slim
WORKDIR /app
COPY leaderboard.py relay.py ./
ENV LEADERBOARD_DB=/data/leaderboard.sqlite3 \
    LEADERBOARD_PORT=8797 \
    PYTHONUNBUFFERED=1
VOLUME /data
EXPOSE 8797
HEALTHCHECK --interval=30s --timeout=3s CMD python -c "import urllib.request;urllib.request.urlopen('http://localhost:8797/healthz')"
CMD ["python", "leaderboard.py"]
