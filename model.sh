tmux kill-session -t aios-mind
tmux new-session -d -s aios-mind "cd /repos/mcp-tools/aios && AIOS_MODEL_PROVIDER=antigravity-cli AIOS_MODEL=gemini-3.6-flash-medium python3 metal/mind-wire.py /tmp/aios-mind.sock"
