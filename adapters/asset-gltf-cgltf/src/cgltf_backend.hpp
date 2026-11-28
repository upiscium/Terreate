#ifndef TERREATE_ADAPTER_ASSET_GLTF_CGLTF_BACKEND_HPP
#define TERREATE_ADAPTER_ASSET_GLTF_CGLTF_BACKEND_HPP

// The single place that includes cgltf. Private to this Adapter: it is not installed and no
// public header includes it. Both translation units that see cgltf (the decoder and the
// implementation unit) include this header, so the declarations and the implementation always
// agree on the feature set.
//
// The Adapter uses `cgltf_parse` on bytes in memory and `cgltf_free` only. It never calls
// `cgltf_parse_file`, `cgltf_load_buffers`, `cgltf_load_buffer_base64`, `cgltf_validate` or the
// accessor readers of cgltf: external resources go through asset::ResolveSession, data URIs are
// decoded by the Adapter, and every bound is checked by the Adapter itself (the cgltf readers
// dereference unaligned pointers and cgltf_validate does not check every index).
//
// cgltf's own allocation goes through the budgeted allocator passed in `cgltf_options::memory`,
// so no `CGLTF_MALLOC` override is needed.

#include <cgltf.h>

#endif // TERREATE_ADAPTER_ASSET_GLTF_CGLTF_BACKEND_HPP
