
### 读流程
```mermaid
sequenceDiagram
  participant Client as Client (caller)
  participant Master as Master (metadata)
  participant HotCache as LocalHotCache
  participant TE as TransferSubmitter/TransferEngine
  participant Remote as RemoteSegment (store node)

  Client->>Master: GetReplicaList(key)  // master_client_.GetReplicaList
  Master-->>Client: ReplicaList + lease_ttl
  Client->>Client: Select replica (FindFirstCompleteReplica / GetPreferredReplica)
  alt hot-cache hit (local)
    Client->>HotCache: GetHotKey(key)  // RedirectToHotCache
    HotCache-->>Client: addr, size
    Client->>TE: (local read) submit READ with local addr -> returns future
  else not in hot cache or remote
    Client->>TE: submit READ to replica.transport_endpoint (remote) -> returns future
    TE->>Remote: transfer request (RDMA/TCP/NOF/... )
    Remote-->>TE: data (zero-copy/segmented)
  end
  TE-->>Client: future->get() (blocks until transfer completes)
  Client->>HotCache: (optional) ProcessSlicesAsync -> submit async put to hot cache
  Client->>Client: Check lease TTL; return success / LEASE_EXPIRED
  ```

### 写流程
```mermaid
sequenceDiagram
  participant Client as Client (caller)
  participant Master as Master (metadata)
  participant TE as TransferSubmitter/TransferEngine
  participant Remote as RemoteSegment(s)
  participant Storage as StorageBackend (local disk thread)

  Client->>Master: PutStart(key, slice_lengths, config)
  Master-->>Client: ReplicaDescriptors (memory/noF/disk)
  par handle disk replica (if any)
    Client->>Storage: PutToLocalFile (D2H copy then async StoreObject)
    Storage-->>Storage: StoreObject (async thread)
    Storage-->>Master: (async) PutEnd(key, DISK)  // when store succeeds
    Storage-->>Master: (if store fails) PutRevoke(key, DISK)
  end
  par write memory/nof replicas
    Client->>TE: submit WRITE to each replica -> returns futures
    TE->>Remote: write transfer (RDMA/TCP/NOF)
    Remote-->>TE: ack (transfer complete)
    TE-->>Client: future->get()  // Client waits for completions
  end
  Client->>Client: DetermineFinalizeDecision (based on transfer_summary)
  alt finalize success
    Client->>Master: PutEnd(key, MEMORY/NOF/ALL)
  else finalize revoke
    Client->>Master: PutRevoke(key, MEMORY/NOF/ALL)
  end
  Client->>Client: return success / error
```