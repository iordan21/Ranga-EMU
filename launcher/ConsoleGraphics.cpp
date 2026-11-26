#include "ConsoleGraphics.hpp"
#include "AlphaExtra.hpp"

namespace Ranga {

namespace {

using Values = std::vector<std::pair<const char*, const char*>>;

const Values kOnOff = {{"auto", "Automático"}, {"enabled", "Ligado"}, {"disabled", "Desligado"}};
const Values kTrueFalse = {{"auto", "Automático"}, {"true", "Ligado"}, {"false", "Desligado"}};

// Ranga's own picture options, for the consoles whose frames Ranga draws (2D and the lighter 3D ones).
const GraphicsOption kFilter = {"Imagem", "ranga_filtro", "Filtro da imagem",
                                {{"auto", "Automático (pixel nítido)"}, {"nitido", "Pixel nítido"}, {"suave", "Suave"}, {"crt", "TV antiga (linhas de CRT)"}, {"pixel", "Pixel puro (sem nada)"}},
                                "Como a imagem do console é aumentada até a sua tela. Pixel nítido deixa os pixels quadrados e limpos; TV antiga imita as linhas de uma TV de tubo. Vale ao reabrir."};
const GraphicsOption kScale = {"Imagem", "ranga_escala", "Tamanho na tela",
                               {{"auto", "Automático (preencher)"}, {"preencher", "Preencher mantendo o formato"}, {"inteira", "Escala inteira (pixel perfeito)"}, {"esticar", "Esticar na tela toda"}},
                               "Escala inteira deixa todos os pixels do mesmo tamanho, com uma borda preta em volta. Vale ao reabrir."};

const std::vector<GraphicsOption> kNes = {
    kFilter, kScale,
    {"Imagem", "mesen_ntsc_filter", "Sinal de TV",
     {{"auto", "Automático (nenhum)"}, {"Disabled", "Nenhum (imagem limpa)"}, {"Composite (Blargg)", "Composto (como na TV)"}, {"S-Video (Blargg)", "S-Video"}, {"RGB (Blargg)", "RGB"}},
     "Imita o cabo da TV da época: cores e bordas como eram no aparelho."},
    {"Imagem", "mesen_palette", "Cores",
     {{"auto", "Automático"}, {"Default", "Padrão"}, {"Nes Classic", "NES Classic"}, {"Nestopia (RGB)", "Nestopia (RGB)"}, {"Original Hardware (by FirebrandX)", "Console original"}},
     "Paleta de cores do NES."},
    {"Desempenho", "mesen_nospritelimit", "Sem limite de sprites", kOnOff, "Tira o pisca-pisca quando há muitos personagens na tela (o console original piscava)."},
};

const std::vector<GraphicsOption> kSnes = {
    kFilter, kScale,
    {"Imagem", "snes9x_aspect", "Proporção", {{"auto", "Automático (4:3)"}, {"4:3", "4:3"}, {"uncorrected", "Pixels quadrados"}, {"ntsc", "NTSC"}, {"pal", "PAL"}},
     "Formato da imagem."},
    {"Imagem", "snes9x_overscan", "Bordas", {{"auto", "Automático"}, {"enabled", "Cortar as bordas"}, {"disabled", "Mostrar tudo"}}, "Corta a faixa da borda que a TV escondia."},
    {"Imagem", "snes9x_hires_blend", "Mistura em alta resolução", {{"auto", "Automático (desligada)"}, {"disabled", "Desligada"}, {"merge", "Misturar"}, {"blur", "Desfocar"}},
     "Alguns jogos (Kirby's Dream Land 3) usam transparência que só aparece misturando."},
    {"Desempenho", "snes9x_reduce_sprite_flicker", "Menos pisca-pisca", kOnOff, "Tira o pisca-pisca com muitos personagens na tela."},
};

const std::vector<GraphicsOption> kMegaDrive = {
    kFilter, kScale,
    {"Imagem", "genesis_plus_gx_blargg_ntsc_filter", "Sinal de TV",
     {{"auto", "Automático (nenhum)"}, {"disabled", "Nenhum"}, {"composite", "Composto (cachoeiras transparentes)"}, {"svideo", "S-Video"}, {"rgb", "RGB"}},
     "Composto imita o cabo da época: as cachoeiras e sombras que \"piscavam\" ficam transparentes, como na TV."},
    {"Imagem", "genesis_plus_gx_aspect_ratio", "Proporção", {{"auto", "Automático"}, {"4:3", "4:3"}, {"Uncorrected", "Pixels quadrados"}}, "Formato da imagem."},
    {"Imagem", "genesis_plus_gx_overscan", "Bordas", {{"auto", "Automático (sem)"}, {"disabled", "Sem bordas"}, {"full", "Com as bordas da TV"}}, "Mostra a borda que a TV mostrava."},
    {"Imagem", "genesis_plus_gx_lcd_filter", "Rastro de LCD (Game Gear)", kOnOff, "Imita o rastro da tela do Game Gear."},
};

const std::vector<GraphicsOption> kPce = {
    kFilter, kScale,
    {"Imagem", "pce_fast_palette", "Cores", {{"auto", "Automático (RGB)"}, {"RGB", "RGB"}, {"Composite", "Composto (como na TV)"}}, "Paleta do console."},
    {"Desempenho", "pce_fast_nospritelimit", "Sem limite de sprites", kOnOff, "Tira o pisca-pisca com muitos personagens na tela."},
};

const std::vector<GraphicsOption> kAtari = {
    kFilter, kScale,
    {"Imagem", "stella_filter", "Sinal de TV", {{"auto", "Automático (nenhum)"}, {"disabled", "Nenhum"}, {"composite", "Composto"}, {"s-video", "S-Video"}, {"rgb", "RGB"}},
     "Imita o cabo da TV da época."},
    {"Imagem", "stella_phosphor_blend", "Rastro do fósforo", {{"auto", "Automático"}, {"0", "0"}, {"30", "30"}, {"60", "60 (padrão)"}, {"80", "80"}},
     "Mistura um quadro com o anterior, como o brilho que ficava na tela da TV (tira o pisca-pisca)."},
};

const std::vector<GraphicsOption> kArcade = {kFilter, kScale};

const std::vector<GraphicsOption> kGba = {
    kFilter, kScale,
    {"Imagem", "mgba_color_correction", "Cores da tela", {{"auto", "Automático (sem correção)"}, {"OFF", "Cores fortes"}, {"GBA", "Como a tela do GBA"}, {"Auto", "Pelo jogo"}},
     "A tela do GBA era escura e lavada; os jogos usavam cores fortes para compensar. Como a tela do GBA deixa as cores como eram vistas."},
    {"Imagem", "mgba_interframe_blending", "Rastro de LCD",
     {{"auto", "Automático (desligado)"}, {"OFF", "Desligado"}, {"mix", "Misturar quadros"}, {"lcd_ghosting", "Rastro da tela"}},
     "Alguns jogos usam o rastro da tela para transparências (piscando a cada quadro)."},
};

const std::vector<GraphicsOption> kGb = {
    kFilter, kScale,
    {"Imagem", "gambatte_gb_colorization", "Cores do Game Boy", {{"auto", "Automático"}, {"disabled", "Verde original"}, {"GBC", "Como no Game Boy Color"}, {"SGB", "Como no Super Game Boy"}},
     "Jogos do Game Boy clássico eram de 4 tons; o Color e o Super Game Boy davam cores."},
    {"Imagem", "gambatte_gbc_color_correction", "Correção de cor (GBC)", {{"auto", "Automático"}, {"GBC only", "Só no Color"}, {"always", "Sempre"}, {"disabled", "Desligada"}},
     "Deixa as cores como eram na tela do Game Boy Color."},
};

const std::vector<GraphicsOption> kSaturn = {
    kFilter, kScale,
    {"Imagem", "beetle_saturn_horizontal_blend", "Suavizar na horizontal", kOnOff, "Mistura os pixels lado a lado: imita as transparências que os jogos faziam com pontilhado."},
};

const std::vector<GraphicsOption> kPs1 = {
    {"Imagem", "swanstation_GPU_ResolutionScale", "Resolução",
     {{"auto", "Automático (pela Qualidade)"}, {"1", "1x (240p, original)"}, {"2", "2x (480p)"}, {"3", "3x (720p)"}, {"4", "4x (960p)"}, {"5", "5x (1080p)"}, {"6", "6x (1440p)"}, {"8", "8x (4K)"}},
     "Quantas vezes a resolução do PS1. Vale ao reabrir."},
    {"Imagem", "swanstation_GPU_TextureFilter", "Filtro de textura",
     {{"auto", "Automático (nenhum)"}, {"Nearest", "Nenhum (original)"}, {"Bilinear", "Bilinear (suave)"}, {"JINC2", "JINC2"}, {"xBR", "xBR (contornos)"}},
     "Suaviza as texturas, que no PS1 eram serrilhadas."},
    {"Imagem", "swanstation_GPU_MSAA", "Antisserrilhado", {{"auto", "Automático (nenhum)"}, {"1", "Nenhum"}, {"2", "MSAA 2x"}, {"4", "MSAA 4x"}, {"2-ssaa", "SSAA 2x (pesado)"}, {"4-ssaa", "SSAA 4x (muito pesado)"}},
     "Suaviza as bordas dos objetos."},
    {"Imagem", "swanstation_GPU_PGXPEnable", "Polígonos firmes (PGXP)", kTrueFalse,
     "Corrige os polígonos que tremiam e se deformavam no PS1. Pode quebrar poucos jogos."},
    {"Imagem", "swanstation_GPU_TrueColor", "Cores de 24 bits", kTrueFalse, "Tira o pontilhado das cores (o PS1 usava 16 bits)."},
    {"Imagem", "swanstation_GPU_WidescreenHack", "Tela larga (16:9)", kTrueFalse, "Força o jogo a desenhar em 16:9. Pode cortar coisas nos menus."},
    {"Imagem", "swanstation_Display_AspectRatio", "Proporção", {{"auto", "Automático (4:3)"}, {"4:3", "4:3"}, {"16:9", "16:9"}, {"Auto", "Pelo jogo"}}, "Formato da imagem."},
    {"Desempenho", "swanstation_GPU_Renderer", "Placa de vídeo usa", {{"auto", "Automático (Vulkan)"}, {"Vulkan", "Vulkan"}, {"D3D11", "Direct3D 11"}, {"OpenGL", "OpenGL"}, {"Software", "Processador (lento)"}},
     "Se der tela preta ou travar, tente Direct3D 11. Vale ao reabrir."},
};

const std::vector<GraphicsOption> kN64 = {
    {"Imagem", "mupen64plus-parallel-rdp-upscaling", "Resolução", {{"auto", "Automático (pela Qualidade)"}, {"1x", "1x (240p, original)"}, {"2x", "2x (480p)"}, {"4x", "4x (960p)"}, {"8x", "8x (pesado)"}},
     "Quantas vezes a resolução do N64. Vale ao reabrir."},
    {"Imagem", "mupen64plus-aspect", "Proporção", {{"auto", "Automático (4:3)"}, {"4:3", "4:3"}, {"16:9 adjusted", "16:9 (tela larga)"}}, "Formato da imagem. Tela larga pode cortar coisas."},
    {"Imagem", "mupen64plus-parallel-rdp-vi-aa", "Antisserrilhado do console", {{"auto", "Automático (ligado)"}, {"True", "Ligado"}, {"False", "Desligado"}},
     "O antisserrilhado que o próprio N64 fazia. Desligado fica mais nítido."},
    {"Imagem", "mupen64plus-parallel-rdp-divot-filter", "Filtro do console (desfoque)", {{"auto", "Automático (ligado)"}, {"True", "Ligado"}, {"False", "Desligado"}},
     "O desfoque típico do N64. Desligado deixa a imagem mais limpa."},
    {"Imagem", "mupen64plus-parallel-rdp-dither-filter", "Tirar pontilhado", {{"auto", "Automático (ligado)"}, {"True", "Ligado"}, {"False", "Desligado"}}, "Esconde o pontilhado das cores."},
};

const std::vector<GraphicsOption> kPsp = {
    {"Imagem", "ppsspp_internal_resolution", "Resolução",
     {{"auto", "Automático (pela Qualidade)"}, {"480x272", "1x (272p, original)"}, {"960x544", "2x (544p)"}, {"1440x816", "3x (816p)"}, {"1920x1088", "4x (1088p)"}, {"2880x1632", "6x (1632p)"}, {"3840x2176", "8x (4K)"}},
     "Vale ao reabrir."},
    {"Imagem", "ppsspp_texture_anisotropic_filtering", "Filtragem anisotrópica", {{"auto", "Automático (pela Qualidade)"}, {"disabled", "Nenhuma"}, {"4x", "4x"}, {"16x", "16x"}},
     "Texturas do chão mais nítidas de longe."},
    {"Imagem", "ppsspp_texture_scaling_level", "Aumentar texturas", {{"auto", "Automático (não)"}, {"disabled", "Não"}, {"2x", "2x"}, {"3x", "3x"}, {"4x", "4x (pesado)"}},
     "Redesenha as texturas maiores (xBRZ). Deixa desenhos e textos mais lisos."},
    {"Imagem", "ppsspp_texture_filtering", "Filtro de textura", {{"auto", "Automático"}, {"Auto", "Pelo jogo"}, {"Nearest", "Nenhum (pixelado)"}, {"Linear", "Linear (suave)"}}, "Suaviza as texturas."},
    {"Desempenho", "ppsspp_frameskip", "Pular quadros", {{"auto", "Automático (não)"}, {"disabled", "Não"}, {"1", "1"}, {"2", "2"}}, "Para PC fraco: desenha menos quadros para o jogo não ficar lento."},
    {"Desempenho", "ppsspp_lower_resolution_for_effects", "Efeitos em resolução menor",
     {{"auto", "Automático (não)"}, {"disabled", "Não"}, {"Safe", "Seguro"}, {"Balanced", "Equilibrado"}, {"Aggressive", "Agressivo"}}, "Desenha brilhos e sombras menores: mais leve."},
};

const std::vector<GraphicsOption> kDreamcast = {
    {"Imagem", "reicast_internal_resolution", "Resolução",
     {{"auto", "Automático (pela Qualidade)"}, {"640x480", "1x (480p, original)"}, {"960x720", "1,5x (720p)"}, {"1440x1080", "2,25x (1080p)"}, {"1920x1440", "3x (1440p)"}, {"2880x2160", "4,5x (4K)"}},
     "Vale ao reabrir."},
    {"Imagem", "reicast_anisotropic_filtering", "Filtragem anisotrópica", {{"auto", "Automático (pela Qualidade)"}, {"off", "Nenhuma"}, {"4", "4x"}, {"16", "16x"}},
     "Texturas do chão mais nítidas de longe."},
    {"Imagem", "reicast_texupscale", "Aumentar texturas", {{"auto", "Automático (não)"}, {"1", "Não"}, {"2", "2x"}, {"4", "4x (pesado)"}}, "Redesenha as texturas maiores (xBRZ)."},
    {"Imagem", "reicast_widescreen_cheats", "Tela larga (códigos)", kOnOff, "Tela larga sem defeitos nos jogos que têm código para isso."},
    {"Imagem", "reicast_widescreen_hack", "Tela larga (forçada)", kOnOff, "Força 16:9 em qualquer jogo; pode aparecer coisa fora do lugar nas bordas."},
    {"Desempenho", "reicast_auto_skip_frame", "Pular quadros", {{"auto", "Automático (não)"}, {"disabled", "Não"}, {"some", "Um pouco"}, {"more", "Mais"}},
     "Para PC fraco: pula quadros quando o jogo ficaria lento."},
};

const std::vector<GraphicsOption> kPs2 = {
    {"Imagem", "pcsx2_upscale_multiplier", "Resolução", {{"auto", "Automático (pela Qualidade)"}, {"1x (Native)", "1x (480p, original)"}, {"2x", "2x (960p)"}, {"4x", "4x (4K, pesado)"}},
     "Vale ao reabrir."},
    {"Imagem", "pcsx2_anisotropic_filtering", "Filtragem anisotrópica", {{"auto", "Automático (nenhuma)"}, {"disabled", "Nenhuma"}, {"4x", "4x"}, {"16x", "16x"}},
     "Texturas do chão mais nítidas de longe."},
    {"Imagem", "pcsx2_texture_filtering", "Filtro de textura",
     {{"auto", "Automático (como o PS2)"}, {"Bilinear (PS2)", "Como o PS2"}, {"Bilinear (Forced)", "Sempre suave"}, {"Nearest", "Nenhum (pixelado)"}}, "Suavização das texturas."},
    {"Imagem", "pcsx2_widescreen_hint", "Tela larga", {{"auto", "Automático (não)"}, {"disabled", "Não"}, {"enabled (16:9)", "16:9"}, {"enabled (21:9)", "21:9"}},
     "Usa os códigos de tela larga do jogo, quando existem."},
    {"Imagem", "pcsx2_deinterlace_mode", "Entrelaçamento", {{"auto", "Automático"}, {"Off", "Desligado"}, {"Blend TFF", "Misturar"}, {"Adaptive TFF", "Adaptativo"}},
     "Tira o tremor de linhas que alguns jogos têm."},
    {"Desempenho", "pcsx2_blending_accuracy", "Precisão das transparências",
     {{"auto", "Automático (pela Qualidade)"}, {"Minimum", "Mínima (mais leve)"}, {"Basic", "Básica"}, {"Medium", "Média"}, {"High", "Alta (pesada)"}}, "Mais alta corrige brilhos e sombras, mas pesa."},
};

const std::vector<GraphicsOption> kDolphin = {
    {"Imagem", "dolphin_efb_scale", "Resolução",
     {{"auto", "Automático (pela Qualidade)"}, {"1", "1x (480p, original)"}, {"2", "2x (720p)"}, {"3", "3x (1080p)"}, {"4", "4x (1440p)"}, {"6", "6x (4K)"}}, "Vale ao reabrir."},
    {"Imagem", "dolphin_max_anisotropy", "Filtragem anisotrópica", {{"auto", "Automático (pela Qualidade)"}, {"0", "Nenhuma"}, {"2", "4x"}, {"4", "16x"}},
     "Texturas do chão mais nítidas de longe."},
    {"Imagem", "dolphin_widescreen_hack", "Tela larga (forçada)", kOnOff, "Força 16:9 em jogos que não têm; pode aparecer coisa cortada nas bordas."},
    {"Imagem", "dolphin_force_texture_filtering_mode", "Filtro de textura", {{"auto", "Automático (pelo jogo)"}, {"0", "Pelo jogo"}, {"1", "Nenhum (pixelado)"}, {"2", "Sempre suave"}},
     "Suavização das texturas."},
    {"Desempenho", "dolphin_shader_compilation_mode", "Shaders",
     {{"auto", "Automático (pela Qualidade)"}, {"0", "Esperar (trava na 1ª vez)"}, {"1", "Ubershaders (sem travar, pesado)"}, {"2", "Em segundo plano"}, {"3", "Em segundo plano, pula o quadro (mais leve)"}},
     "Como os efeitos novos são preparados. Em segundo plano tira as travadas."},
};

const std::vector<GraphicsOption> k3ds = {
    {"Imagem", "citra_resolution_factor", "Resolução",
     {{"auto", "Automático (pela Qualidade)"}, {"1", "1x (240p, original)"}, {"2", "2x (480p)"}, {"3", "3x (720p)"}, {"4", "4x (960p)"}, {"6", "6x (1440p)"}}, "Vale ao reabrir."},
    {"Imagem", "citra_texture_filter", "Filtro de textura",
     {{"auto", "Automático (nenhum)"}, {"none", "Nenhum"}, {"Anime4K", "Anime4K (desenhos)"}, {"Bicubic", "Bicúbico"}, {"ScaleForce", "ScaleForce"}, {"xBRZ", "xBRZ"}, {"MMPX", "MMPX (pixel art)"}},
     "Redesenha as texturas maiores."},
    {"Imagem", "citra_layout_option", "Telas", {{"auto", "Automático (uma em cima da outra)"}, {"default", "Uma em cima da outra"}, {"single_screen", "Só uma tela"}, {"large_screen", "Tela grande + pequena"}, {"side_by_side", "Lado a lado"}},
     "Como as duas telas do 3DS aparecem."},
    {"Imagem", "citra_swap_screen", "Tela principal", {{"auto", "Automático (a de cima)"}, {"Top", "A de cima"}, {"Bottom", "A de baixo"}}, "Qual tela fica em destaque."},
    {"Desempenho", "citra_use_disk_shader_cache", "Guardar shaders no disco", kOnOff, "Guarda os shaders prontos para a próxima vez: menos travadas."},
};

const std::vector<GraphicsOption> kDs = {
    kFilter, kScale,
    {"Imagem", "melonds_opengl_resolution", "Resolução (3D)", {{"auto", "Automático (pela Qualidade)"}, {"1", "1x (original)"}, {"2", "2x"}, {"4", "4x"}, {"6", "6x"}},
     "Só para o desenho em 3D (OpenGL). Vale ao reabrir."},
};


}

const std::vector<GraphicsOption>* ConsoleGraphics(const std::wstring& system) {
    static const std::pair<const wchar_t*, const std::vector<GraphicsOption>*> tables[] = {
        {L"NES", &kNes}, {L"SNES", &kSnes}, {L"MD", &kMegaDrive}, {L"SMS", &kMegaDrive}, {L"SEGACD", &kMegaDrive}, {L"PCE", &kPce},
        {L"A2600", &kAtari}, {L"ARCADE", &kArcade}, {L"GBA", &kGba}, {L"GB", &kGb}, {L"SATURN", &kSaturn}, {L"PS1", &kPs1},
        {L"N64", &kN64}, {L"PSP", &kPsp}, {L"DC", &kDreamcast}, {L"PS2", &kPs2}, {L"GC", &kDolphin}, {L"WII", &kDolphin},
        {L"3DS", &k3ds}, {L"NDS", &kDs},
    };
    for (const auto& [id, table] : tables) {
        if (system == id) return table;
    }
#ifdef RANGA_ALPHA_EXTRA
    return AlphaExtra::Graphics(system);
#else
    return nullptr;
#endif
}

}
