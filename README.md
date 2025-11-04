# mc-sync

a beautifully simple git-like tool for syncing / backing up minecraft worlds

it'll make sense if you look into the code.

#### install

```bash
git clone https://github.com/cappuch/mc-sync.git
cd mc-sync
make
```

#### usage


client = 
```bash
./mcsync init <host> <port>
./mcsync list
./mcsync push <world_dir> [world_name]
./mcsync pull <world_name> <destination_dir>
```

server =
```bash
./mcsync-server -d <storage_dir> [-p port]
```
