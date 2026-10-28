# ROM checksums

The games run from the **chip files** inside MAME's zips, not from the zips themselves. A zip's own checksum changes with how
it was packed, so compare the chip files: if yours match the lists below, you are running the same ROMs as the authors.

Each list is also a file in `md5sum` format, so on Linux you can check a whole game at once. Unpack first (the games do this
themselves the first time they start; the folder is named under each game), then, for example:

```bash
cd dirtdash/extracted && md5sum -c ../../docs/roms/dirtdash.md5
```

On Windows, in PowerShell, inside the folder that holds the chips:

```powershell
Get-FileHash -Algorithm MD5 *
```

(or `certutil -hashfile <file> MD5` for one file), and compare with the table. The **CRC32** column is the one MAME's own
ROM lists show.

Rave Racer's, Ace Driver's, Cyber Commando's and Ridge Racer 2's `c74.bin` (the sound chip's BIOS) comes from `namcoc74.zip`. The DSP's BIOS, `c71.bin` (MAME's `namcoc71`), is built into every System 22 game and
is not needed; if you have it, its MD5 is `223914888d9be9ffe07952d9aa4c4107` (CRC32 `47c623ab`).

## Prop Cycle

MAME set: `propcycl` (World, PR2 Ver.A). Unpacked chips end up in `extracted/`. 28 files.

| File | Size | MD5 | CRC32 |
|---|---:|---|---|
| `pr1ccrh.1d` | 524288 | `a857f962142f0fd127a825545e5ad318` | `1d68bc31` |
| `pr1ccrl.3d` | 2097152 | `df470abb93c504bc0bebf375e5c2610b` | `e01321fd` |
| `pr1cg0.12b` | 2097152 | `0ed6948004a1908bde046675719a08b7` | `0a041238` |
| `pr1cg1.10d` | 2097152 | `78363c16ea46a3eab94451f7f29762c4` | `7d09e6a7` |
| `pr1cg2.12d` | 2097152 | `98370bc7007db8632608da516d05227a` | `659f006e` |
| `pr1cg3.13d` | 2097152 | `df74b926df4e1330e02cae736dfc5cba` | `d30bffa3` |
| `pr1cg4.14d` | 2097152 | `635db3e8823bfcd3bcc62c2169f6104b` | `f4636cc9` |
| `pr1cg5.16d` | 2097152 | `b2de0fe3f1807559b794dd2ce9076c96` | `97d333de` |
| `pr1cg6.18a` | 2097152 | `6a1234afe531f677fc01a63433d0a792` | `3e081c03` |
| `pr1cg7.15a` | 2097152 | `d0ef791af5d899a7ac14c105a5a5c6d9` | `ec9fc5c8` |
| `pr1data.8k` | 524288 | `035621374f8d88d47bd7d210bec7f3f5` | `2e5767a4` |
| `pr1ptrl0.18k` | 524288 | `2e9a7ee6659279502ed20f6e7b2efdae` | `fddb27a2` |
| `pr1ptrl1.16k` | 524288 | `8ac95ad3ba1de14a040873ce8f187de5` | `6964dd06` |
| `pr1ptrl2.15k` | 524288 | `caab9b4f3a21df6b2eb6f520de956c63` | `4d7ed1d4` |
| `pr1ptrm0.18j` | 524288 | `ee2e617f7ce4b6e0caf7e6cc361397b0` | `b6f204b7` |
| `pr1ptrm1.16j` | 524288 | `d508b9d66fc33a6bf908de63aea0a99b` | `949588b7` |
| `pr1ptrm2.15j` | 524288 | `1d2ef206c495ec1cab0ae1635d6b6eef` | `dc1cef0a` |
| `pr1ptru0.18f` | 524288 | `226806ad705df011044b7b1693d82756` | `5d66a7c4` |
| `pr1ptru1.16f` | 524288 | `07f08ac88693da3ca3bcd9c83e78c10f` | `e9a3f72b` |
| `pr1ptru2.15f` | 524288 | `cbec24ec682982fca74559633a7ef934` | `c346a842` |
| `pr1scg0.12f` | 2097152 | `7b45b300da60d0d2c70852595876e6d7` | `2d09a869` |
| `pr1scg1.10f` | 2097152 | `7a3ac71716198743dce8de38feef6474` | `7433c5bd` |
| `pr1wavea.2l` | 4194304 | `b3378e0978e0b9e4cf66a34a0f44aed1` | `320f3913` |
| `pr1waveb.1l` | 4194304 | `4dbfa6fe13e9e40377b0f0857d632e8f` | `d91acb26` |
| `pr2ver-a.1` | 1048576 | `9e7104e844b8d01f4adaca7582421648` | `3f58594c` |
| `pr2ver-a.2` | 1048576 | `1dd8aa60232b837bc56706e135a9c279` | `c0da354a` |
| `pr2ver-a.3` | 1048576 | `56d0d8771375ebb83ef9b5a4af4e5c9c` | `74bf4b74` |
| `pr2ver-a.4` | 1048576 | `8939346b64c361eb24d8c47aee334952` | `cf4d5638` |

## Rave Racer

MAME set: `raverace` (World, RV2 Ver.B) and `namcoc74`. Unpacked chips end up in `raverace/extracted/`. 36 files.

| File | Size | MD5 | CRC32 |
|---|---:|---|---|
| `c74.bin` | 16384 | `c78a2c4d6071a227e6d49bc009ef7600` | `a3dce360` |
| `rr1gam.2d` | 256 | `8554aa5b8d9b2bfac5222d3391de25f0` | `b2161bce` |
| `rr1gam.3d` | 256 | `8554aa5b8d9b2bfac5222d3391de25f0` | `b2161bce` |
| `rr1gam.4d` | 256 | `8554aa5b8d9b2bfac5222d3391de25f0` | `b2161bce` |
| `rv1ccrh.5c` | 524288 | `c643b58bb0ee2a71c96c3906982b4743` | `a741b262` |
| `rv1ccrl.5a` | 2097152 | `efdd77905c59e08744a08e2fed204e26` | `bc634f72` |
| `rv1cg0.1a` | 2097152 | `90b5089dae9fc3e0bda5e9a5db7b6b5b` | `c518f06b` |
| `rv1cg1.1c` | 2097152 | `d39b32a856eb0bc52a5e3ffa8089de3f` | `6628f792` |
| `rv1cg2.1d` | 2097152 | `e632db51ba9ad3cd93d2e52ccab5de05` | `0b707cc5` |
| `rv1cg3.1e` | 2097152 | `2261cb3d7f51378b6d66b65675632d58` | `39b62921` |
| `rv1cg4.1f` | 2097152 | `00c5eed967cc9e4e0ae236ea72e2f81a` | `a9791ea2` |
| `rv1cg5.1j` | 2097152 | `e4a60f7cc6bd1d42c4d9c7ea0efdaee5` | `b2c79ec1` |
| `rv1cg6.1k` | 2097152 | `bfc63298d453fb619eb970707df80f06` | `8cddedc2` |
| `rv1cg7.1n` | 2097152 | `37cd1dd9f787c88c2c5e7a6950cb49bc` | `b39147ca` |
| `rv1data.6r` | 524288 | `1b6940a91f8d9977c4685c567f3d1b17` | `d358ec20` |
| `rv1eeprm.9e` | 8192 | `1680290e11ced23a4be4e63c31adbc27` | `e00dd412` |
| `rv1potl0.5b` | 524288 | `af1d38846deed52063a79ccbb1a99d9d` | `de2ce519` |
| `rv1potl1.4b` | 524288 | `b8605991f19ab385743888ad1f90c11e` | `2215cb5a` |
| `rv1potl2.3b` | 524288 | `5c2916472435748551665464c8ae6855` | `ddb15bf7` |
| `rv1potl3.2b` | 524288 | `5cfd1204d24028b9baafa8795066d449` | `fa9361ca` |
| `rv1potm0.5c` | 524288 | `516783d75aa6cc6e99d11b520f7bddf3` | `3c024f3a` |
| `rv1potm1.4c` | 524288 | `9bd3b922f7c114d47f6d3ee32670eddb` | `b1a32a68` |
| `rv1potm2.3c` | 524288 | `ee4874e0f964e94e67ecc931dfecfada` | `a414fe15` |
| `rv1potm3.2c` | 524288 | `e0c3e33f610f16ad290f995832b1a1ca` | `2953bbb4` |
| `rv1potu0.5d` | 524288 | `56bb59bbeb17654779a35900015e74e0` | `b9eaf3cc` |
| `rv1potu1.4d` | 524288 | `0a929531264dd8fe8ed6c44c46ac9c82` | `a5c55258` |
| `rv1potu2.3d` | 524288 | `036aef350756c6a577941ab30dad996b` | `c18fcb74` |
| `rv1potu3.2d` | 524288 | `48a5ebb8ebb045723fa53512ffbc38ae` | `79735aaa` |
| `rv1wav0.10r` | 1048576 | `6b4c5bdf146bc97a5caaf79e09bdb37e` | `5aef8143` |
| `rv1wav1.10p` | 1048576 | `2133eb12a2ca59bb2265275ec84b1b73` | `9ed9e6b3` |
| `rv1wav2.10n` | 1048576 | `f171a8dea29b5a72bf239a99950be7c6` | `5af9dc83` |
| `rv1wav3.10l` | 1048576 | `5f126c54f8c2fd9187c296800abe7cd3` | `ffb9ad75` |
| `rv2_prgllb.4d` | 524288 | `76498f25cdc70611dc85823b1d349ff4` | `3017cd1e` |
| `rv2_prglmb.2d` | 524288 | `0ef49ad4beb013ea4c613eccb15dfa80` | `894be0c3` |
| `rv2_prgumb.8d` | 524288 | `377153d7ed0a4eec956cfafc6317b3c5` | `6414a800` |
| `rv2_prguub.6d` | 524288 | `c76d5f5731b7e9e7994ac3f28db57db9` | `a9f18714` |

## Tokyo Wars

MAME set: `tokyowar` (World, TW2 Ver.A). Unpacked chips end up in `tokyowar/extracted/`. 33 files.

| File | Size | MD5 | CRC32 |
|---|---:|---|---|
| `tokyowar_defaults.nv` | 8192 | `e84f6a0ad1bc545d6783d89587337515` | `e8bd7d09` |
| `tw1ccrh.1d` | 524288 | `28fc6d58ba0ca37c3cf3727717d71613` | `ad17e693` |
| `tw1ccrl.3d` | 2097152 | `1e3143cd9728b392f786512616e8982f` | `d08f5794` |
| `tw1cg0.8d` | 2097152 | `5ddaf85ace13c11dfb9c2eaa61cd6378` | `98b9b070` |
| `tw1cg1.10d` | 2097152 | `559f69101b21d9086966f4cbb3d20c67` | `f96a723a` |
| `tw1cg2.12d` | 2097152 | `80ad1684ca14c04723240e8a50426025` | `573e9ded` |
| `tw1cg3.13d` | 2097152 | `cdaf6f767cc7700e3c06b9321b5d75b6` | `302d5c74` |
| `tw1cg4.14d` | 2097152 | `5330d8ad7c1906d470fa364f8216a334` | `ab8aa1df` |
| `tw1cg5.16d` | 2097152 | `a7293131b56990b122b692f35493103b` | `5063f3d0` |
| `tw1cg6.18d` | 2097152 | `408d68b902a491669e4724d89af5c418` | `d764027c` |
| `tw1cg7.19d` | 2097152 | `ea304a368b9dd5b7b2635c9d29744c28` | `12da0a43` |
| `tw1data.8k` | 524288 | `101678e4d132ebfa08b2e43383c59aba` | `bd046e4b` |
| `tw1ptrl0.18k` | 524288 | `6a3355abb08b5a9024d32c3aa7b2146f` | `44ac5e86` |
| `tw1ptrl1.16k` | 524288 | `344a3b9f9f35ae933102eddbaf768638` | `3c769860` |
| `tw1ptrl2.15k` | 524288 | `5d55166b8f403c5ab825875be80f000a` | `6e94103c` |
| `tw1ptrl3.14k` | 524288 | `78df1224db126a6b0657a3841b2866c0` | `e3ce5eb2` |
| `tw1ptrm0.18j` | 524288 | `7f78431b1f0198ff03b4f26e0e5ae6ae` | `e170cea2` |
| `tw1ptrm1.16j` | 524288 | `6369cb27b6da7419865362ef592f479c` | `36a32237` |
| `tw1ptrm2.15j` | 524288 | `c04a752b7be9f2be763241f74946ac2d` | `c426c278` |
| `tw1ptrm3.14j` | 524288 | `7bfbf62759d46e1d1d9b96abe8d3a600` | `d9b9a651` |
| `tw1ptru0.18f` | 524288 | `a302bb75aa891831fbfc92d698ea121e` | `62a9e9fb` |
| `tw1ptru1.16f` | 524288 | `0b7eae13aae322e7fcf9c3ca56e04c59` | `2fd36177` |
| `tw1ptru2.15f` | 524288 | `bc06c1a6af142ce6691baabb68951f02` | `ceacb1c9` |
| `tw1ptru3.14f` | 524288 | `5acc757ea3b43de250764cd9583014d4` | `939044c2` |
| `tw1scg0.12f` | 2097152 | `0f4371ce9c1dfffab1c175b5c4ac9217` | `e3ec4daa` |
| `tw1scg1.10f` | 2097152 | `b5b426813304de23f8b6481de67ac774` | `b18a06e9` |
| `tw1scg2.8f` | 2097152 | `e93fc2e058aa736394049b32ecaf12d1` | `36f8c3d8` |
| `tw1scg3.7f` | 2097152 | `375b822d18fbe0e19e984ab6c5dac2b5` | `8e14d013` |
| `tw1wavea.2l` | 4194304 | `58f5c78ae0a8a2a6b6447f5b6c670ca5` | `ebce6366` |
| `tw2ver-a.1` | 1048576 | `85ae48af51e37e6844208e935447a572` | `2b17ca92` |
| `tw2ver-a.2` | 1048576 | `768a1697e89ba3d4c886b452d510cfe1` | `12da84e3` |
| `tw2ver-a.3` | 1048576 | `15460c962deb86216f00c5619d3d7c0b` | `7d42c516` |
| `tw2ver-a.4` | 1048576 | `b12da5b948b6ea7404777e54e221ac9d` | `b904ed16` |

## Dirt Dash

MAME set: `dirtdash` (World, DT2 Ver.A, MAME's `dirtdasha`). Unpacked chips end up in `dirtdash/extracted/`. 26 files.

| File | Size | MD5 | CRC32 |
|---|---:|---|---|
| `dt1ccrh.1d` | 524288 | `5acabd9bead8907cc8b9da4577e37fbf` | `af257064` |
| `dt1ccrl.3d` | 2097152 | `677daaf8a0483c40835d326d46879300` | `e536b313` |
| `dt1cg0.8d` | 2097152 | `65fbe1567cbb9bf2ff5f32fce0116b5b` | `10ab95e0` |
| `dt1cg1.10d` | 2097152 | `858938d8e58a42ab1e033a58710b6dae` | `d9f1ba53` |
| `dt1cg2.12d` | 2097152 | `609a485d2fca9831e697d8f12ea40bab` | `bd8b1e0b` |
| `dt1cg3.13d` | 2097152 | `2a93301bd87c89a20ffb21de20a75851` | `ba960663` |
| `dt1cg4.14d` | 2097152 | `edcc176f3f2332a73eb1250c33366acf` | `424b9652` |
| `dt1cg5.16d` | 2097152 | `9fb68279f181bfb553a25e7ab7aa3a1a` | `29516626` |
| `dt1cg6.18d` | 2097152 | `8af0ebc3be5419aa85be700d0c60f146` | `e6fa7180` |
| `dt1cg7.19d` | 2097152 | `4433c54422673d8f70ab7184fdfb07e5` | `2ca19153` |
| `dt1dataa.8k` | 524288 | `e477767ed49bdab9d6d7deca34a205bb` | `9bcdea21` |
| `dt1ptrl0.18k` | 524288 | `06c6b3710173c07c1ad8f38eebfd9b97` | `4e0cac3a` |
| `dt1ptrl1.16k` | 524288 | `60ecb0415ddb709c8b0ec11cf539a543` | `59ba9dba` |
| `dt1ptrl2.15k` | 524288 | `09c35244cd23a57deea755ec34078ed9` | `cfe80c67` |
| `dt1ptrm0.18j` | 524288 | `e012b770b344038126d2544dcad6fcb2` | `41f34337` |
| `dt1ptrm1.16j` | 524288 | `dd5b1c48ad96cc1568d01cad7b737bfd` | `f620fd41` |
| `dt1ptrm2.15j` | 524288 | `90de422b5cfe0e352fbb2f4961fab5d1` | `71e6714d` |
| `dt1ptru0.18f` | 524288 | `fd6b86daae8939fd64aae399291a6663` | `4909bd7d` |
| `dt1ptru1.16f` | 524288 | `1311f227590a6ceec93a88f1058b2702` | `4a5097df` |
| `dt1ptru2.15f` | 524288 | `a91c9e24f1b356cd7ec3ecbdd8ee0168` | `1171eaf5` |
| `dt1scg0.12f` | 2097152 | `0b1f79c01ba136432cc1a2974cc33acb` | `a09b5760` |
| `dt1scg1.10f` | 2097152 | `86f95a2b68fbb3b08b2d95be56dae961` | `f9ac8111` |
| `dt1wavea.2l` | 4194304 | `0eb373b848bb30069853412fd9d5c309` | `cbd52e40` |
| `dt1waveb.1l` | 4194304 | `1280e17788bfcf83c6e5bf125f37a4a6` | `6b736f94` |
| `dt2vera.1` | 2097152 | `4fbb2b5ead26717087ac638e0d57aee1` | `402a3d73` |
| `dt2vera.2` | 2097152 | `3ca9ef45fb4e0663b2d15040dcd7e9d2` | `66ed140d` |

## Armadillo Racing

MAME set: `adillor` (World, AM2 Ver.A). Unpacked chips end up in `adillor/extracted/`. 32 files.

| File | Size | MD5 | CRC32 |
|---|---:|---|---|
| `am1ccrh.1d` | 524288 | `d5fb81ff68aaa52a0f166513347562e5` | `180c4a33` |
| `am1ccrl.3d` | 2097152 | `87d65ff0dc9a35d6298b8ac65d772963` | `76c6d5f3` |
| `am1cg0.8d` | 2097152 | `b30a6bf683580193efea0261c1e84e98` | `b91e4259` |
| `am1cg1.10d` | 2097152 | `08680c23bc747ffdc34bf4f61ee4eb33` | `c1b82a26` |
| `am1cg2.12d` | 2097152 | `272d10a0b5806be0f3e313e9580bbcf4` | `36a7d3fd` |
| `am1cg3.13d` | 2097152 | `40804790e6d2e4b33ce114e864c1b1a5` | `8b0857ef` |
| `am1cg4.14d` | 2097152 | `5ecf2aca39f50e11b07e430d639049b5` | `ebc516fe` |
| `am1cg5.16d` | 2097152 | `cac82d9e363a41100a91599a7dbe888c` | `30fad30d` |
| `am1cg6.18d` | 2097152 | `996ae3137a53ad521cc9cff0a0666c47` | `62eea883` |
| `am1cg7.19d` | 2097152 | `8052e1b1cd1119af7480fa009fd98b6c` | `2d257f47` |
| `am1data.8k` | 524288 | `9ade8ac0652121ccb654a26ab91903df` | `3c176589` |
| `am1ptrl0.18k` | 524288 | `b2840e122e3d2ba7fdb901af0683bb6f` | `6fdb34f6` |
| `am1ptrl1.16k` | 524288 | `73206053ab654e4d4e516956524d15d9` | `32e40601` |
| `am1ptrl2.15k` | 524288 | `e1b113b7cbfaff8398a02d72bfe461af` | `e27e29cc` |
| `am1ptrl3.14k` | 524288 | `8d963aa792ee7767e0d6e61e601c0c10` | `0c638e3e` |
| `am1ptrm0.18j` | 524288 | `9b70e1239158890759da011477abe0a1` | `e676cd98` |
| `am1ptrm1.16j` | 524288 | `46456fc5d785c52d3c58daace886ca44` | `eb115d9d` |
| `am1ptrm2.15j` | 524288 | `eab9b487f9af71562a02a9ab612c7e07` | `86b4ed0c` |
| `am1ptrm3.14j` | 524288 | `25afc759d29588bbfa13cd6473c9686b` | `97c1c6b0` |
| `am1ptru0.18f` | 524288 | `b2ebec378bf6c028ca6d056afc0cb16f` | `6a3fd855` |
| `am1ptru1.16f` | 524288 | `4c49be5098dad0710a14a6400b89e64f` | `551bf714` |
| `am1ptru2.15f` | 524288 | `a10ab4d0ed3c31f589184dac8a670665` | `ce790635` |
| `am1ptru3.14f` | 524288 | `555ed74e8fd7ac68147d01f0295b4383` | `5e4e6333` |
| `am1scg0.12f` | 2097152 | `59b93f070b37ad5acd7fdb4058b71ac7` | `e576f993` |
| `am1scg1.10f` | 2097152 | `134a7ec4ff103a0250b34432b3f2c415` | `d6d26cc1` |
| `am1wavea.2l` | 4194304 | `02ce6e8a13c5710f64c10767aa24b883` | `48f8c20c` |
| `am1waveb.1l` | 4194304 | `c3f297ef25831c08348e875cdbcbbf46` | `fd8e7384` |
| `am2vera.rom1` | 1048576 | `191baf7c4cc78d895ecea10658253775` | `b974e931` |
| `am2vera.rom2` | 1048576 | `3a462c35a83a032c1525ca829c64be80` | `c7cd78f1` |
| `am2vera.rom3` | 1048576 | `516b4ee7ca8555af2185577f9948fabe` | `80e9435a` |
| `am2vera.rom4` | 1048576 | `11607d78512e33da46c30f3d3763791b` | `1e3a6032` |
| `c71.bin` | 8192 | `223914888d9be9ffe07952d9aa4c4107` | `47c623ab` |

## Time Crisis

MAME set: `timecris` (World, TS2 Ver.B). Unpacked chips end up in `timecris/extracted/`. 31 files.

| File | Size | MD5 | CRC32 |
|---|---:|---|---|
| `ts1ccrh.1d` | 524288 | `b7361fceb8032178d7c9bb5f66836a6d` | `a1cc3741` |
| `ts1ccrl.3d` | 2097152 | `5de5b2e6c5d76bf1c6d2a8fe47686887` | `56cad2df` |
| `ts1cg0.8d` | 2097152 | `262f35748078b6595dc7e956f26917e1` | `de07b22c` |
| `ts1cg1.10d` | 2097152 | `c4baee28395da3b1d3cf9fa6b67b1bd5` | `992d26f6` |
| `ts1cg2.12d` | 2097152 | `195e5de3ae6ac3a9d328bc77828933bd` | `6273954f` |
| `ts1cg3.13d` | 2097152 | `836d56de3db96cfcfbdac67a7fd1db2e` | `38171f24` |
| `ts1cg4.14d` | 2097152 | `655063f241c7f3d4dc007f370c1c1c85` | `51f09856` |
| `ts1cg5.16d` | 2097152 | `1c920eea8d0c6b6690760db69191b226` | `4cd9fd79` |
| `ts1cg6.18d` | 2097152 | `be4859f31e72d909a69ce98e1f8c4612` | `f17f2ec9` |
| `ts1data.8k` | 524288 | `f3da24336095ab2314f7b865c38acfd1` | `e68aa973` |
| `ts1ptrl0.18k` | 524288 | `6a3f5a92d0cbe6f2356d9e78a54e59ce` | `e5f2d275` |
| `ts1ptrl1.16k` | 524288 | `f0a45a495350d157d635e1dd2cdebe06` | `2bba3800` |
| `ts1ptrl2.15k` | 524288 | `139bcbf1623ed20099b55d6f432ed03c` | `d4441c08` |
| `ts1ptrm0.18j` | 524288 | `b2c0603a4ca77b2200c04429e3900b94` | `8aea02ba` |
| `ts1ptrm1.16j` | 524288 | `49345ef3915d0c57e069fd63035b80f0` | `bccf19bc` |
| `ts1ptrm2.15j` | 524288 | `03acc19a71dcd6e34985a3343fbaa435` | `7280be31` |
| `ts1ptru0.18f` | 524288 | `5bce3e37a27ac9a6d7bd5bf85b615b5e` | `c30d6332` |
| `ts1ptru1.16f` | 524288 | `f1382f97c76434f6dde2c896389a7b82` | `993cde84` |
| `ts1ptru2.15f` | 524288 | `830cf203aba845f2d21c7d8412255524` | `7cb25c73` |
| `ts1scg0.12f` | 2097152 | `e9dc10ef2b3dbc6ed1e9cc6690890906` | `14a3674d` |
| `ts1scg1.10f` | 2097152 | `eba66d3ffa2b08deb1828fc2981c83ad` | `11791dbf` |
| `ts1scg2.8f` | 2097152 | `1b21d707ec62ff0be909add851f5e16a` | `d630fff9` |
| `ts1scg3.7f` | 2097152 | `bbe6e72aa658b77925b6a476fd086f0b` | `1a62f015` |
| `ts1scg4.5f` | 2097152 | `ff61f50c457c1830a4ce5def9361bc67` | `511b8dd6` |
| `ts1scg5.3f` | 2097152 | `98b3615f7613b15fcfe8a6baabfc5fd4` | `553bb246` |
| `ts1wavea.2l` | 4194304 | `e8ed56dafba5716a4adac46e68df4d8e` | `d1123301` |
| `ts1waveb.1l` | 2097152 | `77a80c8a938bc08c8a80f141cde540c4` | `bf4d7272` |
| `ts2verb.1` | 1048576 | `5d9efdf70acfb2eb4d8b6a2dca24afb3` | `29b377f7` |
| `ts2verb.2` | 1048576 | `3b849adff914dd1d6eca3ee103cc7e90` | `79512e25` |
| `ts2verb.3` | 1048576 | `140924f0d62359b0fe1f47999a82f8f5` | `9f4ced33` |
| `ts2verb.4` | 1048576 | `0cbfc47b60ccbdbcf34743fb82b4a054` | `3e0cfb38` |

## Ace Driver

MAME set: `acedrive` (World, AD2) and `namcoc74`. Unpacked chips end up in `acedriver/extracted/`. 25 files.

| File | Size | MD5 | CRC32 |
|---|---:|---|---|
| `ad1ccrh.2c` | 524288 | `08bcfaaca7718cf520615753478cd123` | `71f44526` |
| `ad1ccrl.1c` | 2097152 | `bae70d5bcb3a158b4621bee1f93be31d` | `bc3c9b12` |
| `ad1cg0.1a` | 2097152 | `499b542cd6af8c296891190d003b7a41` | `faaa1ee2` |
| `ad1cg1.2a` | 2097152 | `1c45646cbf3b17bc9d1c0134ae215c13` | `1aab1eb7` |
| `ad1cg2.3a` | 2097152 | `5bf949953a1a31fabf1a0c6138770fbe` | `cdcd1874` |
| `ad1cg3.5a` | 2097152 | `3fab9a4a875b1dbbb0dbd343224199aa` | `effdd2cd` |
| `ad1data.6r` | 524288 | `357a681ecf1c6a34e188edc18d72e814` | `82024f74` |
| `ad1potl0.5b` | 524288 | `977e9096c74c1c801aa05e6e8650d469` | `dfc7e729` |
| `ad1potl1.4b` | 524288 | `32d8e8166a669995022ac55f510c6707` | `5914ef8e` |
| `ad1potm0.5c` | 524288 | `0cbd2c87745fffbfad8df84728a391ef` | `844bcd6b` |
| `ad1potm1.4c` | 524288 | `4cc41f3a3523ce0da02431ad05b03b2f` | `515cf541` |
| `ad1potu0.5d` | 524288 | `e6b8370dc1fd74056ab3a5bca580a970` | `e0f44949` |
| `ad1potu1.4d` | 524288 | `02a0cf71a8d81202efd871407524cd10` | `f2cd2cbb` |
| `ad1wave0.10r` | 1048576 | `90ee6498b3c979cec5328e79edfa900a` | `c7879a72` |
| `ad1wave1.10p` | 1048576 | `949c08ca9531eed176696d973b55e2f3` | `69c1d41e` |
| `ad1wave2.10n` | 1048576 | `2374566aff5bcc6da0ba5fb667e41bc4` | `365a6831` |
| `ad1wave3.10l` | 1048576 | `fdcc951cc1692d8088c36952b5375fb8` | `cd8ecb0b` |
| `ad2_prgll.4d` | 524288 | `a28d99b23e375253d0ab37c34e3281e6` | `808c5ff8` |
| `ad2_prglm.2d` | 524288 | `22744662d82da82d6b4c30e9da70832d` | `5f726a10` |
| `ad2_prgum.8d` | 524288 | `684a824ad89e894d1598a68c5fdc47a4` | `d5042d6e` |
| `ad2_prguu.6d` | 524288 | `c72cc24705dcc9ab70e762c754882a5d` | `86d4661d` |
| `c74.bin` | 16384 | `c78a2c4d6071a227e6d49bc009ef7600` | `a3dce360` |
| `rr1gam.2d` | 256 | `8554aa5b8d9b2bfac5222d3391de25f0` | `b2161bce` |
| `rr1gam.3d` | 256 | `8554aa5b8d9b2bfac5222d3391de25f0` | `b2161bce` |
| `rr1gam.4d` | 256 | `8554aa5b8d9b2bfac5222d3391de25f0` | `b2161bce` |

## Cyber Commando

MAME set: `cybrcomm` (Japan, CY1) and `namcoc74`. Unpacked chips end up in `cybrcomm/extracted/`. 29 files.

| File | Size | MD5 | CRC32 |
|---|---:|---|---|
| `c74.bin` | 16384 | `c78a2c4d6071a227e6d49bc009ef7600` | `a3dce360` |
| `cy1data.6r` | 131072 | `7d0229144ae524798933f4397203e582` | `10d0005b` |
| `cy1eeprm.9e` | 8192 | `b55429e7f9e256c77f6bc69776984cbb` | `8432c066` |
| `cy1prgll.4d` | 524288 | `ffd6c73cd2c5ab95166f7ec8bf911db0` | `b3eab156` |
| `cy1prglm.2d` | 524288 | `3fde321dc526ca85b5ea42b6af0e4d0a` | `884a5b0e` |
| `cy1prgum.8d` | 524288 | `fa6ae7f1a2375024596a83ade432616a` | `c9c4a921` |
| `cy1prguu.6d` | 524288 | `9dce98ee02e03c3072bd387271cb54f4` | `5f22975b` |
| `cy1wav0.10r` | 1048576 | `d1f3f6329bbafe903b87698a9a1a6fc1` | `c6f366a2` |
| `cy1wav1.10p` | 1048576 | `7b81c108a4e8499722d1a63d02708a65` | `f30b5e37` |
| `cy1wav2.10n` | 1048576 | `b35a0ff75bd2533692bacf6db088651c` | `b98c1ca6` |
| `cy1wav3.10l` | 1048576 | `897fb7339cabb8fb49580d60bf2b2c0d` | `43dbac19` |
| `cyc1ccrh.2c` | 524288 | `487ee1864a71fd837c178e81c66e9735` | `8c4090b8` |
| `cyc1ccrl.1c` | 1048576 | `ed2f59b192da16a1c97084cb36509858` | `1a0dc5f0` |
| `cyc1cg0.1a` | 2097152 | `4c11b15a96469c9d79440745575a5b86` | `e839b9bd` |
| `cyc1cg1.2a` | 2097152 | `202937f712218f43eb882d26fe0e7dc0` | `7d13993f` |
| `cyc1cg2.3a` | 2097152 | `6a58757210b6ecb6741b47e29ba53529` | `7c464566` |
| `cyc1cg3.5a` | 2097152 | `6a820d29d7f6c86bfd92e3f61d5d4f85` | `2222e16f` |
| `cyc1ptl0.5b` | 524288 | `6b48d68982c809cc0feca6dfd90006d3` | `d91de03d` |
| `cyc1ptl1.4b` | 524288 | `44c6f72666d200b9b45cfa3c11e39928` | `e5b98021` |
| `cyc1ptl2.3b` | 524288 | `624cf2e87186ca8faa031d4fb8d4ee5d` | `7ba786c6` |
| `cyc1ptm0.5c` | 524288 | `1418066bcd3884ad50898a0ec0f7b7e6` | `d454b5c6` |
| `cyc1ptm1.4c` | 524288 | `f0203e60f431882e17ef87040ff6a24b` | `74fdf8cc` |
| `cyc1ptm2.3c` | 524288 | `a5a0c1a78c24cae3c4a7b8889d335fde` | `b9c99a45` |
| `cyc1ptu0.5d` | 524288 | `4124672f66326534a36880a44e0e5f1c` | `4d40897f` |
| `cyc1ptu1.4d` | 524288 | `37d3e7f3f4773b08da15d49f0fbe0bac` | `3bdaeeeb` |
| `cyc1ptu2.3d` | 524288 | `ea57ab1d5e80c35cde17285ae42888bf` | `a0e73674` |
| `rr1gam.2d` | 256 | `8554aa5b8d9b2bfac5222d3391de25f0` | `b2161bce` |
| `rr1gam.3d` | 256 | `8554aa5b8d9b2bfac5222d3391de25f0` | `b2161bce` |
| `rr1gam.4d` | 256 | `8554aa5b8d9b2bfac5222d3391de25f0` | `b2161bce` |

## Ridge Racer 2

MAME set: `ridgera2` (World, RRS2) and `namcoc74`. Unpacked chips end up in `ridgera2/extracted/`. 25 files.

| File | Size | MD5 | CRC32 |
|---|---:|---|---|
| `c74.bin` | 16384 | `c78a2c4d6071a227e6d49bc009ef7600` | `a3dce360` |
| `rr1gam.2d` | 256 | `8554aa5b8d9b2bfac5222d3391de25f0` | `b2161bce` |
| `rr1gam.3d` | 256 | `8554aa5b8d9b2bfac5222d3391de25f0` | `b2161bce` |
| `rr1gam.4d` | 256 | `8554aa5b8d9b2bfac5222d3391de25f0` | `b2161bce` |
| `rrs1ccrh.5c` | 524288 | `bd8f35fc178af5291699692ae0361532` | `bd3c86ab` |
| `rrs1ccrl.5a` | 2097152 | `bb1c3638381883609a51a511c707da6b` | `304a8b57` |
| `rrs1cg0.1a` | 2097152 | `befecfc0b75b3e1190164d6afdcce079` | `714c0091` |
| `rrs1cg1.2a` | 2097152 | `c2ae0da7518511b0e9ad86ba31469844` | `836545c1` |
| `rrs1cg2.3a` | 2097152 | `ca8efdddabf28808292db41b4ae5735b` | `00e9799d` |
| `rrs1cg3.5a` | 2097152 | `54921ec062f234acfc0dc24799d388d7` | `3858983f` |
| `rrs1data.6r` | 524288 | `e3c36a75ace5a3213a1996590e50794e` | `b7063aa8` |
| `rrs1pol0.5b` | 524288 | `5e9bf8c867a9e9a275fef44f2fec2807` | `9376c384` |
| `rrs1pol1.4b` | 524288 | `72b80c533b294f0ff27b2b808226e4e3` | `094fa832` |
| `rrs1pom0.5c` | 524288 | `a9885cb3e64cb36227c7409c3a064e09` | `b47a7f8b` |
| `rrs1pom1.4c` | 524288 | `2e7c2a7ba26c95816d5fd8cf78f9bd4e` | `27260361` |
| `rrs1pou0.5d` | 524288 | `d246eae609330245a61967c6b0695ca7` | `74d6ec84` |
| `rrs1pou1.4d` | 524288 | `ad04db595df6939c955b4952ff019774` | `f527caaa` |
| `rrs1wav0.10r` | 1048576 | `d25f058bc3c48458938ac25b298721d9` | `99d11a2d` |
| `rrs1wav1.10p` | 1048576 | `fb115463eba9435650474d0ae167cfcf` | `ad28444a` |
| `rrs1wav2.10n` | 1048576 | `73c8c196d3d3e568b90425247316978d` | `6f0d4619` |
| `rrs1wav3.10l` | 1048576 | `22a82a6fc017bb8d7c6b3384386243d4` | `106e761f` |
| `rrs2_prgll.4d` | 524288 | `a0f696df38f666bd6ea67dcd7a811289` | `88199c0f` |
| `rrs2_prglm.2d` | 524288 | `6cf2fe15c5699c97f7e1d2ad2c407e9b` | `8e86f199` |
| `rrs2_prgum.8d` | 524288 | `6353961b75b8a608ad5bf9dc542e6ba5` | `78c360b6` |
| `rrs2_prguu.6d` | 524288 | `c9488dad6c3ca64afc02d62a43882a8c` | `60d6d4a4` |

## Cyber Sled

MAME set: `cybsled` (World, CY2), `namcoc67` and `namcoc68`. Unpacked chips end up in `cybsled/extracted/`. 30 files.

| File | Size | MD5 | CRC32 |
|---|---:|---|---|
| `c67.bin` | 8192 | `35381d235bff6cd22204f4a03eb499f3` | `6bd8988e` |
| `c68.bin` | 32768 | `c9b10b75a476ed686581f86913c2808b` | `ca64550a` |
| `cy1-data-l.1a` | 524288 | `4a1daf6c8c401a365861064d1605ad36` | `9cf96f9e` |
| `cy1-data-u.3a` | 524288 | `6a409d2dc3211ee02d7c04c0af98a6be` | `570da15d` |
| `cy1-edata0-l.1b` | 524288 | `37c1cdadfc397c1d977b6fd1d9d44132` | `e812e290` |
| `cy1-edata0-u.3b` | 524288 | `be21a716f2d5c1bc01c0359cb403b05e` | `77452533` |
| `cy1-obj0.5s` | 524288 | `f0a3338dc9cc13be1857fd5562dde484` | `5ae542d5` |
| `cy1-obj1.5x` | 524288 | `be5ad68592007a237b74841094a93885` | `4aae3eff` |
| `cy1-obj2.3s` | 524288 | `f75e0406062d28da75fe4743611724eb` | `d64ec4c3` |
| `cy1-obj3.3x` | 524288 | `ef82a8e08a9339b81d3944563a272732` | `3d1f7168` |
| `cy1-obj4.4s` | 524288 | `3c8a26b228809b88c3940b5b8ed76887` | `57904076` |
| `cy1-obj5.4x` | 524288 | `cf1e1bdd4759461040820fee7ada2e95` | `0e11ca47` |
| `cy1-obj6.2s` | 524288 | `363459c87d5bff5971391ebb3e0c1c48` | `7748b485` |
| `cy1-obj7.2x` | 524288 | `40095c62701854f2d7cdb90f166a59ed` | `b6eb6ad2` |
| `cy1-poi-h1.2f` | 524288 | `88d177f2c0f0d35aa927f8cf88f902ea` | `eaf8bac3` |
| `cy1-poi-h2.2j` | 524288 | `9c56bbd966d87e1de7abcc5b79dde45e` | `4079f342` |
| `cy1-poi-ll1.2n` | 524288 | `a5360a13ddb26028e5bad8f0f3bc8ba1` | `30acb99b` |
| `cy1-poi-ll2.2p` | 524288 | `be58e23f26cf9eb7dbb62f1a8b08a248` | `faf09158` |
| `cy1-poi-lu1.2k` | 524288 | `bcd462f2fa79e14b6d1e5bb875482cc7` | `c544a8dc` |
| `cy1-poi-lu2.2l` | 524288 | `a2a076a0f165912925d80469ac66174c` | `61d816d4` |
| `cy1-snd0.8j` | 131072 | `8a9bceaf67efddf79eb2f22d7e22c1da` | `3dddf83b` |
| `cy1-voi0.12b` | 524288 | `290e0eb6809dfcee2bd73fcacf3f6f54` | `99d7ce46` |
| `cy1-voi1.12c` | 524288 | `3e815a267a5eb4095d08c27cfea1597e` | `2b335f06` |
| `cy1-voi2.12d` | 524288 | `8f0c438884ca2e4c783b269b30fd8c36` | `10cd15f0` |
| `cy1-voi3.12e` | 524288 | `d83269fb7116a7a6ba009b85e66b139b` | `c902b4a4` |
| `cy2-mpr-l.1j` | 524288 | `9546d08c0c387ce36310c1328f26cfcd` | `c4a25919` |
| `cy2-mpr-u.3j` | 524288 | `98cbc63cbc0f67d554bc9cb63165e051` | `b35a72bc` |
| `cy2-spr-l.4c` | 524288 | `f519aa6495d765722c7daa6c3a69d289` | `4066291a` |
| `cy2-spr-u.6c` | 524288 | `8e28d63abcd9b60147c35d44fafecacd` | `575a422d` |
| `cybsled.nv` | 8192 | `671fef57adc93d69feada9f50f3ad784` | `aa18bf9e` |

## Time Crisis II

MAME set: `timecrs2` (US, TSS3 Ver.B). Unpacked chips end up in `tc2/extracted/`. 22 files.

| File | Size | MD5 | CRC32 |
|---|---:|---|---|
| `tss1ccrh.7e` | 2097152 | `6a42384da3aeb1aa5e38f6015defbb6d` | `f998de1a` |
| `tss1ccrl.7f` | 4194304 | `a1df84856600ebad532c23914afab1c5` | `3a325fe7` |
| `tss1cgll.4m` | 8388608 | `67a3a8563a72dde5ebe98d8886fdb0ba` | `18433aaa` |
| `tss1cglm.4k` | 8388608 | `757b4d3f6ab011cdb4ed813f680fe43a` | `669974c2` |
| `tss1cgum.4j` | 8388608 | `f791c542b157effc7859d6ae78fc5992` | `c22739e1` |
| `tss1cguu.4f` | 8388608 | `f5d94c8c7c2a6e96dfcc01bd2237cf35` | `76924e04` |
| `tss1mtah.2j` | 8388608 | `70124b89b972bcc4157c2ac88a6ce718` | `697c26ed` |
| `tss1mtal.2h` | 8388608 | `8b70fb75c64d251645fa6c0b44ade8d2` | `bfc79190` |
| `tss1mtbh.2m` | 8388608 | `e3a70c5d76dea4e70a96ec47643a274e` | `82582776` |
| `tss1mtbl.2f` | 8388608 | `653caa4af2c6be02f0648baa95d916c3` | `e648bea4` |
| `tss1pt0h.7a` | 4194304 | `ea6e4766e4addb3c48f017821b16b7ce` | `cdbe0ba8` |
| `tss1pt0l.7c` | 4194304 | `01c20b6470b02a2fe90a3b17a8f613c4` | `896f0fb4` |
| `tss1pt1h.5a` | 4194304 | `35bdbb96dbff3645d54ef187ac7f3335` | `63647596` |
| `tss1pt1l.5c` | 4194304 | `208c03507c9239debdc08e0ef1663f22` | `5a09921f` |
| `tss1pt2h.4a` | 4194304 | `d33ba0afb6fe9f1a3104faa6ca656f6e` | `9b06e22d` |
| `tss1pt2l.4c` | 4194304 | `1405a1ad95457cd465e5e522c64a9aa7` | `4b230d79` |
| `tss1vera.3` | 524288 | `91dcca3b700c0f1c626138a1cc44cbd2` | `41e41994` |
| `tss1waveh.2a` | 8388608 | `e0c5f92b23162bac5793332aca9f4423` | `5c8758b4` |
| `tss1wavel.2c` | 8388608 | `4c4b10cbaab533a4badc0d4f28d01b01` | `deaead26` |
| `tss3verb.1` | 2097152 | `825e2529c601b08f1dcbb43016410ee2` | `6e3f232b` |
| `tss3verb.2` | 2097152 | `fe415802a9076f39493861f7de633d46` | `c7be691f` |
| `tssioprog.ic3` | 262144 | `ff39548cd899ac1d01d1a2f96077c840` | `edad4538` |

## Crisis Zone

MAME set: `crszone` (World, CSZO4 Ver.B). Unpacked chips end up in `crszone/extracted/`. 23 files. If MAME keeps the I/O board's program (`csz1prg0a.8f`) in a separate `namco_csz1.zip`, put that beside `crszone.zip`.

| File | Size | MD5 | CRC32 |
|---|---:|---|---|
| `csz1ccrh.7e` | 2097152 | `4e8631e4c5feef7f4bb58d0e503b705e` | `bc2fa03c` |
| `csz1ccrl.7f` | 4194304 | `5618c9356eecdfe37ee2e8d78e561b07` | `1c20768d` |
| `csz1cgll.4m` | 8388608 | `0ac875b81e3d7bd97ae97d150175f230` | `0bcd41f2` |
| `csz1cglm.4k` | 8388608 | `a0e6901e2a6b557bc7e2d6e9ed6fa1b6` | `d4af93d1` |
| `csz1cgum.4j` | 8388608 | `05123f52bf7faef780f2e4e9db86711f` | `913c98b5` |
| `csz1cguu.5f` | 8388608 | `2de8240bd842f8fcaa447b06d166f625` | `e1d1bf24` |
| `csz1mtah.2j` | 8388608 | `29f2acee88a116d7e8d80b6a151927c8` | `66b076ad` |
| `csz1mtal.2h` | 8388608 | `b4b0b9103be7230e35ab5e0c48bbf57d` | `38dc639a` |
| `csz1mtbh.2m` | 8388608 | `6ab0964bb30f6a805d03c5a4602dd276` | `bdec4188` |
| `csz1mtbl.2f` | 8388608 | `c357f961409afc7d4847a130855b3f7e` | `9c8f8d7a` |
| `csz1prg0a.8f` | 131072 | `52d170854516489d99e73c4c915fdefd` | `8edc36b3` |
| `csz1pt0h.7a` | 4194304 | `2d4301665c7c3dc561919e0289bd9b8f` | `e82f1abb` |
| `csz1pt0l.7c` | 4194304 | `95637dbecb45364403a342fb39fc086d` | `b0d66afe` |
| `csz1pt1h.5a` | 4194304 | `faca4b01e136d850493a29bc2ef8c683` | `e54f80ad` |
| `csz1pt1l.5c` | 4194304 | `5773879c23520a77984cc5316481c627` | `527171c8` |
| `csz1pt2h.4a` | 4194304 | `a5286aedab68a3440fae1ce0477be2af` | `e295137a` |
| `csz1pt2l.4c` | 4194304 | `3b083ec880f4c578fede3e864d8da4bd` | `c87d6dbd` |
| `csz1pt3h.3a` | 4194304 | `42609386c6d62dd8a00aa7e21b3d2a4c` | `05f65bdf` |
| `csz1pt3l.3c` | 4194304 | `86a0f31c0fb58838b99e66147d9c30d4` | `5d077c0f` |
| `csz1waveh.2a` | 8388608 | `1440ee5df5da8a16070ce47475c49ff0` | `de9d14a8` |
| `csz1wavel.2c` | 8388608 | `1edc748d021925313bca7393c021a4b6` | `d0d74132` |
| `cszo3verb.ic1` | 524288 | `e5eff660b92812891cf6110f8806f9f0` | `c790743b` |
| `cszo4verb.ic4` | 8388608 | `821af14d9188a0e464ab5c12c63a8a9d` | `6192533d` |

## Angler King

MAME set: `aking` (Japan, AG1 Ver.A). Unpacked chips end up in `aking/extracted/`. 24 files. The I/O board's program is the revision `aking.zip` carries, `asc3_io-c1.ic14` (MAME 0.289's `namco_fca10` device lists `asc3_io-c.ic14`; that one is not used).

| File | Size | MD5 | CRC32 |
|---|---:|---|---|
| `ag1ccrh.7e` | 2097152 | `7b2cb3a9793f489962270a1eb7986563` | `abe2aab1` |
| `ag1ccrl.7f` | 4194304 | `494b01a6910e7c1bc0223c6feecf2816` | `86bbe1f9` |
| `ag1cgll.4m` | 8388608 | `bca574b4fd8108917b4c4c3709bf05fa` | `9db7e939` |
| `ag1cglm.4k` | 8388608 | `f34ab3a3bf1effdb292dd956d51fe0e1` | `17792dba` |
| `ag1cgum.4j` | 8388608 | `244a0b0be4df87574624c39e9c71ac91` | `5dfa863d` |
| `ag1cguu.4f` | 8388608 | `84083ef74aca72375080842eab0a1b30` | `86396786` |
| `ag1mtah.2j` | 8388608 | `07e3bb43457cfa2bead42ce8d3f8c955` | `f2d8ca9d` |
| `ag1mtal.2h` | 8388608 | `60312dbbb12bb1cc6e229f26e48a0e16` | `7facbfd4` |
| `ag1mtbh.2m` | 8388608 | `27298a48d20addca79aa355ed5259b99` | `890bdb52` |
| `ag1mtbl.2f` | 8388608 | `bfb15a4e6aa035009707628a79ba80d5` | `62d771c9` |
| `ag1pt0h.7a` | 4194304 | `e15b06fd03974898ec988978073fe5fd` | `b5582ca7` |
| `ag1pt0l.7c` | 4194304 | `ff95bab878c61fdc2debefb67dc6acf4` | `10e7f54f` |
| `ag1pt1h.5a` | 4194304 | `0f584e9836a7f640443eb50c0905c676` | `25e4776a` |
| `ag1pt1l.5c` | 4194304 | `07c88b45ef01f69d17a2014b9fe4dac5` | `5d3d7099` |
| `ag1pt2h.4a` | 4194304 | `acc7111268d57ca89180e34face3136d` | `f0eb9012` |
| `ag1pt2l.4c` | 4194304 | `9e9d79760c46daa22e4be585a6a9eff3` | `bf92c054` |
| `ag1pt3h.3a` | 4194304 | `aaf7e0efe6dfadc9ef6c2d559ec7f24e` | `e11a12ce` |
| `ag1pt3l.3c` | 4194304 | `a61576a6f2ffeb05499a1f4c060e116c` | `04b475db` |
| `ag1vera.ic1` | 2097152 | `4230a9e1c105f3ae1e923c57c2f85640` | `f1a08d5c` |
| `ag1vera.ic2` | 2097152 | `3751cc4125b3839fde5c8b9bc35d86ad` | `dc98fefb` |
| `ag1vera.ic3` | 524288 | `4e5ff95bc3b5dd3d6b42b24fa47cb4d8` | `266ac71c` |
| `ag1waveh.2a` | 8388608 | `409c99191e47dedaaa8b272ce104fca3` | `37a61daa` |
| `ag1wavel.2c` | 8388608 | `ac8cb58f1a4a602b2448694281f9c62c` | `d7fefbd4` |
| `asc3_io-c1.ic14` | 131072 | `162005b32c4b3ddb9afa772bffb97505` | `b6627a0e` |
