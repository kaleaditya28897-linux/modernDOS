/* iPXE in MDVMM: data that iPXE's headers declare in ways this file must
   not see (compiled without them).

   virt_offset is "const" for iPXE's code; the monitor sets it once, before
   anything of iPXE runs: physical address = virtual address + virt_offset
   for everything in the monitor's pool. */
unsigned long virt_offset;

void ipxe_set_virt_offset(unsigned long offset)
{
  virt_offset = offset;
}

/* objects the drivers "require" (iPXE's configuration objects) */
char obj_config_ethernet, obj_config_pci, obj_config_net80211;
