"""Specialize the existing DSP instruction semantics for a user's microcode.

The output is ROM-derived and belongs under ignored build/gen/, never source.
The default validates the complete image when the bus revision changes;
unrecognized images and debugger hooks use guarded individual instructions.
"""
import hashlib,pathlib,struct,sys
source,out=map(pathlib.Path,sys.argv[1:3]);data=source.read_bytes()
if len(data)!=4096*4:raise ValueError('Expected all 4096 TGP program words')
words=struct.unpack('<4096I',data)
out.parent.mkdir(parents=True,exist_ok=True)
lines=[f'// Generated from the player microcode SHA-256 {hashlib.sha256(data).hexdigest()}.',
       '// ROM-derived: keep this file in ignored build/gen/.',
       'static const compiled_entry entries[4096]={']
matrix_matches=hashlib.sha256(data[0xbc3*4:0xbfa*4]).hexdigest()=='34a521c91986cfcfb0b0b7977dd2ba3f01f9dd168e930913273282dc80cbc7ba'
reverse_matrix_matches=hashlib.sha256(data[0xbfb*4:0xc35*4]).hexdigest()=='93543014c3797bf41699a46ac4dcee02c11cb57b90b942b54f2748a230182639'
command_table=all((word&0xffff0000)==0xbf600000 for word in words[0x3f:0xaf])
rotations={
    0xb79:(3,6,'769220eb4ccf8ffc43e102661bab42b44dc626561ce3cd06863b44e80c30dc07'),
    0xb93:(6,0,'a0bacb6e2dbefec4691dff9f07bdc029e33f045eddd5067e48c4aa2f22071d79'),
    0xbad:(0,3,'7e6568d444c550d10d8e40910f4422ac3a9181cef6e3a37f471fd7f6bf1a5f42'),
}
rotation_matches={
    pc: (first,second)
    for pc,(first,second,digest) in rotations.items()
    if hashlib.sha256(data[pc*4:(pc+20)*4]).hexdigest()==digest
}
translations={
    0xb31:(0xb42,False,'646eeb0ab72a93aea11ee14c823c964537f16f7869afeabd624635852ab94ab8'),
    0xb44:(0xb5b,True,'7f329a2d595978419a772b37a8a06dd06f50f194865751ac1e8d2fccfba85cc4'),
}
translation_matches={
    pc:(last,write_matrix)
    for pc,(last,write_matrix,digest) in translations.items()
    if hashlib.sha256(data[pc*4:(last+1)*4]).hexdigest()==digest
}
# Straight-line private regions of the supported 0x6b and 0x4a commands.
# The C++ specialization validates every opcode before eliding boundaries.
private_regions = {
    0x757: 0x773, 0x777: 0x780,
    0xd83: 0xd91, 0xd98: 0xdaa, 0xdb0: 0xdc1, 0xdca: 0xdeb, 0xdf9: 0xe04,
}
# End each command 0x32 region before the next FIFO or board callback.
command32_regions = {
    0x4b5: 0x4bb, 0x4bd: 0x4c2, 0x4c4: 0x4c8, 0x4c9: 0x4cb, 0x4cc: 0x4df,
}
private_image = hashlib.sha256(data).hexdigest() == "6c3858903f6f0f701f76cfe43ef542ab5b1aea759875cbe79ae4b6c8765c813d"
dispatch_matches = command_table and words[0x36:0x3f] == (
    0x1c1f1813, 0x1c1f200c, 0x1c1f3214, 0x1f7f1e10, 0xfe0401af,
    0x1c1f320c, 0x3900003f, 0x1f5f1e10, 0xbf624019,
)
for pc,word in enumerate(words):
    handler='matrix_entry' if (matrix_matches and pc==0xbc3) or (reverse_matrix_matches and pc in (0xbfb,0xbfc)) else f'compiled_instruction<0x{word:08x}u>'
    if pc in rotation_matches:
        first,second=rotation_matches[pc]
        handler=f'rotation_entry<{first},{second},0x{word:08x}u>'
    if pc in translation_matches:
        _,write_matrix=translation_matches[pc]
        handler=f'translation_entry<{str(write_matrix).lower()},0x{word:08x}u>'
    # These integer MOV variants can batch a repeat inside validated direct RAM.
    if word in (0x1c1343a1,0x1c0f43a1,0x1c0f4200,0x1c1c21a1):
        handler=f'compiled_repeat_move<0x{word:08x}u>'
    if command_table and (pc==0x35 or 0x3f<=pc<0xaf):
        command=255 if pc==0x35 else pc-0x3f
        handler=f'command_entry<{command},0x{word:08x}u>'
    if dispatch_matches and words[0x35] == 0x1c1f2621 and pc == 0x35:
        handler = 'command_intake_entry'
    if dispatch_matches and pc == 0x36:
        handler = 'command_dispatch_entry'
    if dispatch_matches and 0x3b<=pc<=0x3e:
        handler = f'command_suffix_entry<0x{pc:02x},0x{word:08x}u>'
    if private_image and pc in private_regions:
        sequence = ','.join(f'0x{opcode:08x}u' for opcode in words[pc:private_regions[pc]])
        handler = f'private_region_entry<0x{pc:03x},{sequence}>'
    if private_image and matrix_matches and pc == 0x0290:
        handler = 'command37_setup_entry'
    if private_image and matrix_matches and pc == 0x0297:
        handler = 'command37_finish_entry'
    if private_image and pc in command32_regions:
        sequence = ','.join(f'0x{opcode:08x}u' for opcode in words[pc:command32_regions[pc]])
        handler = f'command32_region_entry<0x{pc:03x},{sequence}>'
    if private_image and pc == 0xdcc:
        # Command 4a typically reaches the original 0xdca head with just
        # one or two cycles left. Skip the first two operations, preserving
        # their ordinary handlers before trying the rest of the region.
        sequence = ','.join(f'0x{opcode:08x}u' for opcode in words[pc:private_regions[0xdca]])
        handler = f'private_4a_resume_entry<0x{pc:03x},{sequence}>'
    lines.append(f'    {{0x{word:08x}u,&{handler}}},')
lines.extend(['};','return entries;',''])
out.write_text('\n'.join(lines));print(f'{len(set(words))} distinct instruction handlers, {len(words)} guarded program entries -> {out}')
blocks=out.with_name('vf2_tgp_blocks.inc')
lines=['// ROM-derived revision-validated instruction blocks; keep under ignored build/gen/.',
       'static const compiled_block_entry entries[512]={']
for at in range(0,4096,8):
    instructions=','.join(f'0x{word:08x}u' for word in words[at:at+8])
    lines.append(f'    {{&compiled_block<{instructions}>}},')
lines.extend(['};','return entries;','']);blocks.write_text('\n'.join(lines));print(f'512 eight-word blocks -> {blocks}')

timing=out.with_name('vf2_tgp_matrix_timing.inc')
lines=['// ROM-derived original cycle prefixes for recognized private matrix arithmetic.',
       'static const matrix_timing entries[]={']
timed_regions=[(0xbc3,0xbf9,matrix_matches),(0xbfb,0xc34,reverse_matrix_matches),
               (0xbfc,0xc34,reverse_matrix_matches)]
timed_regions.extend((pc,pc+19,True) for pc in rotation_matches)
timed_regions.extend((pc,last,True) for pc,(last,_) in translation_matches.items())
for first,last,recognized in timed_regions:
    if not recognized:continue
    prefix=[0]
    for opcode in words[first:last+1]:
        family=opcode>>26;alu=(opcode>>21)&31
        cost=2 if family in (0,7) and alu in (5,6,7,8,9,10,11,12,13,16,17,19,20) else 1
        prefix.append(prefix[-1]+cost)
    lines.append(f'    {{0x{first:03x},{last-first+1},{{'+','.join(map(str,prefix))+'}},')
lines.extend(['    {0,0,{0}}','};','for(const auto &t:entries)if(t.instructions&&t.entry==entry)return &t;','return nullptr;',''])
timing.write_text('\n'.join(lines));print(f'Native matrix timing prefixes -> {timing}')
