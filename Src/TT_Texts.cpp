// *****************************************************************************
// Tapir Translator - texts of the tool's own interface (implementation, see TT_Texts.hpp)
//
//   ROLE IN THE SYSTEM
//   The table below holds one row per TT::Text (same order as the enum, each
//   row preceded by a comment naming it) and 20 columns, one per interface
//   language: fr en de es pt it hu cs pl ru uk el tr nl sv no fi ja ko zh.
//   Placeholders such as {v} or {l} are filled in by TxtF.
//   No straight double quote inside the texts: the menus are generated from this
//   table into GRC resources. Translations still to be reviewed by native speakers.
// *****************************************************************************

#include "TT_Texts.hpp"

namespace TT {

namespace {

// kTexts[text][interface language index], UTF-8.
const char* const kTexts[TextCount][InterfaceLanguageCount] = {
	// M_INPUT
	{ "Langue d'entrée", "Input language", "Eingabesprache", "Idioma de entrada", "Idioma de entrada", "Lingua di origine",
	  "Bemeneti nyelv", "Vstupní jazyk", "Język wejściowy", "Исходный язык", "Мова оригіналу", "Γλώσσα εισόδου", "Giriş dili",
	  "Brontaal", "Källspråk", "Kildespråk", "Lähdekieli", "入力言語", "입력 언어", "输入语言" },
	// M_AUTO
	{ "Détection automatique", "Automatic detection", "Automatische Erkennung", "Detección automática", "Deteção automática",
	  "Rilevamento automatico", "Automatikus felismerés", "Automatická detekce", "Wykrywanie automatyczne", "Автоопределение",
	  "Автовизначення", "Αυτόματος εντοπισμός", "Otomatik algılama", "Automatisch detecteren", "Automatisk identifiering",
	  "Automatisk gjenkjenning", "Automaattinen tunnistus", "自動検出", "자동 감지", "自动检测" },
	// M_OUTPUT
	{ "Langue de sortie", "Output language", "Ausgabesprache", "Idioma de salida", "Idioma de saída", "Lingua di destinazione",
	  "Kimeneti nyelv", "Výstupní jazyk", "Język wyjściowy", "Язык перевода", "Мова перекладу", "Γλώσσα εξόδου", "Çıkış dili",
	  "Doeltaal", "Målspråk", "Målspråk", "Kohdekieli", "出力言語", "출력 언어", "输出语言" },
	// M_NONE
	{ "Aucune traduction", "No translation", "Keine Übersetzung", "Sin traducción", "Sem tradução", "Nessuna traduzione",
	  "Nincs fordítás", "Bez překladu", "Bez tłumaczenia", "Без перевода", "Без перекладу", "Χωρίς μετάφραση", "Çeviri yok",
	  "Geen vertaling", "Ingen översättning", "Ingen oversettelse", "Ei käännöstä", "翻訳しない", "번역 안 함", "不翻译" },
	// M_TRANSLATOR
	{ "Traducteur en ligne", "Online translator", "Online-Übersetzer", "Traductor en línea", "Tradutor online", "Traduttore online",
	  "Online fordító", "Online překladač", "Tłumacz online", "Онлайн-переводчик", "Онлайн-перекладач", "Διαδικτυακός μεταφραστής",
	  "Çevrimiçi çevirmen", "Online vertaler", "Onlineöversättare", "Nettbasert oversetter", "Verkkokääntäjä", "オンライン翻訳",
	  "온라인 번역기", "在线翻译" },
	// M_NO_KEY
	{ "sans clé", "no key", "ohne Schlüssel", "sin clave", "sem chave", "senza chiave", "kulcs nélkül", "bez klíče", "bez klucza",
	  "без ключа", "без ключа", "χωρίς κλειδί", "anahtarsız", "zonder sleutel", "utan nyckel", "uten nøkkel", "ilman avainta",
	  "キー不要", "키 없음", "无需密钥" },
	// M_KEYS
	{ "Clés des traducteurs...", "Translator keys...", "Übersetzer-Schlüssel...", "Claves de los traductores...",
	  "Chaves dos tradutores...", "Chiavi dei traduttori...", "Fordítói kulcsok...", "Klíče překladačů...", "Klucze tłumaczy...",
	  "Ключи переводчиков...", "Ключі перекладачів...", "Κλειδιά μεταφραστών...", "Çevirmen anahtarları...", "Vertaalsleutels...",
	  "Översättarnycklar...", "Oversetternøkler...", "Kääntäjien avaimet...", "翻訳サービスのキー...", "번역기 키...", "翻译服务密钥..." },
	// M_CORRECTIONS
	{ "Corrections des termes...", "Term corrections...", "Begriffskorrekturen...", "Corrección de términos...",
	  "Correção de termos...", "Correzione dei termini...", "Kifejezések javítása...", "Opravy termínů...", "Poprawki terminów...",
	  "Исправление терминов...", "Виправлення термінів...", "Διορθώσεις όρων...", "Terim düzeltmeleri...", "Termcorrecties...",
	  "Termkorrigeringar...", "Termrettelser...", "Termikorjaukset...", "用語の修正...", "용어 수정...", "术语更正..." },
	// M_OPTIONS
	{ "Options", "Options", "Optionen", "Opciones", "Opções", "Opzioni", "Beállítások", "Možnosti", "Opcje", "Параметры",
	  "Параметри", "Επιλογές", "Seçenekler", "Opties", "Alternativ", "Alternativer", "Asetukset", "オプション", "옵션", "选项" },
	// M_ONLINE
	{ "Traduction en ligne (textes hors dictionnaire)", "Online translation (texts not in dictionaries)",
	  "Online-Übersetzung (Texte außerhalb der Wörterbücher)", "Traducción en línea (textos fuera de los diccionarios)",
	  "Tradução online (textos fora dos dicionários)", "Traduzione online (testi non nei dizionari)",
	  "Online fordítás (szótáron kívüli szövegek)", "Online překlad (texty mimo slovníky)", "Tłumaczenie online (teksty spoza słowników)",
	  "Онлайн-перевод (тексты вне словарей)", "Онлайн-переклад (тексти поза словниками)",
	  "Διαδικτυακή μετάφραση (κείμενα εκτός λεξικών)", "Çevrimiçi çeviri (sözlükte olmayan metinler)",
	  "Online vertaling (teksten buiten de woordenboeken)", "Onlineöversättning (texter utanför ordlistorna)",
	  "Nettbasert oversettelse (tekster utenfor ordlistene)", "Verkkokäännös (sanastojen ulkopuoliset tekstit)",
	  "オンライン翻訳（辞書にないテキスト）", "온라인 번역(사전에 없는 텍스트)", "在线翻译（词典以外的文本）" },
	// M_COLLECT
	{ "Collecter les textes non traduits", "Collect untranslated texts", "Nicht übersetzte Texte sammeln",
	  "Recopilar textos sin traducir", "Recolher textos não traduzidos", "Raccogli i testi non tradotti",
	  "Fordítatlan szövegek gyűjtése", "Shromažďovat nepřeložené texty", "Zbieraj nieprzetłumaczone teksty",
	  "Собирать непереведённые тексты", "Збирати неперекладені тексти", "Συλλογή μη μεταφρασμένων κειμένων",
	  "Çevrilmemiş metinleri topla", "Onvertaalde teksten verzamelen", "Samla oöversatta texter", "Samle uoversatte tekster",
	  "Kerää kääntämättömät tekstit", "未翻訳のテキストを収集", "번역되지 않은 텍스트 수집", "收集未翻译的文本" },
	// M_FOLDER
	{ "Ouvrir le dossier des dictionnaires", "Open the dictionaries folder", "Wörterbuchordner öffnen",
	  "Abrir la carpeta de diccionarios", "Abrir a pasta dos dicionários", "Apri la cartella dei dizionari", "Szótármappa megnyitása",
	  "Otevřít složku slovníků", "Otwórz folder słowników", "Открыть папку словарей", "Відкрити папку словників",
	  "Άνοιγμα φακέλου λεξικών", "Sözlük klasörünü aç", "Map met woordenboeken openen", "Öppna mappen med ordlistor",
	  "Åpne mappen med ordlister", "Avaa sanastokansio", "辞書フォルダーを開く", "사전 폴더 열기", "打开词典文件夹" },
	// M_STATUS
	{ "État de Tapir Translator (Rapport de session)", "Tapir Translator status (Session Report)",
	  "Status von Tapir Translator (Sitzungsbericht)", "Estado de Tapir Translator (Informe de sesión)",
	  "Estado do Tapir Translator (Relatório da sessão)", "Stato di Tapir Translator (Rapporto di sessione)",
	  "Tapir Translator állapota (munkamenet-jelentés)", "Stav Tapir Translator (Zpráva relace)", "Stan Tapir Translator (Raport sesji)",
	  "Состояние Tapir Translator (отчёт сеанса)", "Стан Tapir Translator (звіт сеансу)",
	  "Κατάσταση Tapir Translator (Αναφορά συνεδρίας)", "Tapir Translator durumu (Oturum Raporu)",
	  "Status van Tapir Translator (Sessierapport)", "Status för Tapir Translator (Sessionsrapport)",
	  "Status for Tapir Translator (Øktrapport)", "Tapir Translatorin tila (istuntoraportti)",
	  "Tapir Translator の状態（セッションレポート）", "Tapir Translator 상태(세션 보고서)", "Tapir Translator 状态（会话报告）" },
	// M_INTERFACE
	{ "Langue de Tapir Translator", "Tapir Translator language", "Sprache von Tapir Translator", "Idioma de Tapir Translator",
	  "Idioma do Tapir Translator", "Lingua di Tapir Translator", "Tapir Translator nyelve", "Jazyk Tapir Translator",
	  "Język Tapir Translator", "Язык Tapir Translator", "Мова Tapir Translator", "Γλώσσα του Tapir Translator",
	  "Tapir Translator dili", "Taal van Tapir Translator", "Språk för Tapir Translator", "Språk for Tapir Translator",
	  "Tapir Translatorin kieli", "Tapir Translator の言語", "Tapir Translator 언어", "Tapir Translator 语言" },
	// M_INTERFACE_AUTO
	{ "Comme ArchiCAD", "Same as ArchiCAD", "Wie ArchiCAD", "Igual que ArchiCAD", "Igual ao ArchiCAD", "Come ArchiCAD",
	  "Mint az ArchiCAD", "Stejně jako ArchiCAD", "Jak ArchiCAD", "Как в ArchiCAD", "Як в ArchiCAD", "Όπως το ArchiCAD",
	  "ArchiCAD ile aynı", "Zoals ArchiCAD", "Samma som ArchiCAD", "Samme som ArchiCAD", "Kuten ArchiCAD", "ArchiCAD と同じ",
	  "ArchiCAD와 동일", "与 ArchiCAD 相同" },

	// P_AUTO
	{ "Reconnaît tout seul la langue des textes du script.", "Recognizes the language of the script texts automatically.",
	  "Erkennt die Sprache der Skripttexte automatisch.", "Reconoce automáticamente el idioma de los textos del script.",
	  "Reconhece automaticamente o idioma dos textos do script.", "Riconosce automaticamente la lingua dei testi dello script.",
	  "Automatikusan felismeri a szkript szövegeinek nyelvét.", "Automaticky rozpozná jazyk textů skriptu.",
	  "Automatycznie rozpoznaje język tekstów skryptu.", "Автоматически определяет язык текстов скрипта.",
	  "Автоматично визначає мову текстів скрипту.", "Αναγνωρίζει αυτόματα τη γλώσσα των κειμένων του σεναρίου.",
	  "Betik metinlerinin dilini otomatik olarak tanır.", "Herkent automatisch de taal van de scriptteksten.",
	  "Känner automatiskt igen språket i skriptets texter.", "Gjenkjenner automatisk språket i skripttekstene.",
	  "Tunnistaa skriptin tekstien kielen automaattisesti.", "スクリプトのテキストの言語を自動で判別します。",
	  "스크립트 텍스트의 언어를 자동으로 인식합니다.", "自动识别脚本文本的语言。" },
	// P_INPUT
	{ "Langue dans laquelle les scripts sont écrits.", "Language the scripts are written in.",
	  "Sprache, in der die Skripte geschrieben sind.", "Idioma en que están escritos los scripts.",
	  "Idioma em que os scripts estão escritos.", "Lingua in cui sono scritti gli script.", "A szkriptek nyelve.",
	  "Jazyk, ve kterém jsou skripty napsány.", "Język, w którym napisane są skrypty.", "Язык, на котором написаны скрипты.",
	  "Мова, якою написані скрипти.", "Γλώσσα στην οποία είναι γραμμένα τα σενάρια.", "Betiklerin yazıldığı dil.",
	  "Taal waarin de scripts geschreven zijn.", "Språket som skripten är skrivna på.", "Språket skriptene er skrevet på.",
	  "Kieli, jolla skriptit on kirjoitettu.", "スクリプトが書かれている言語。", "스크립트가 작성된 언어입니다.", "脚本所用的语言。" },
	// P_NONE
	{ "Affiche les scripts tels qu'ils sont écrits.", "Shows the scripts as they are written.",
	  "Zeigt die Skripte so, wie sie geschrieben sind.", "Muestra los scripts tal como están escritos.",
	  "Mostra os scripts tal como estão escritos.", "Mostra gli script così come sono scritti.",
	  "A szkripteket eredeti formájukban mutatja.", "Zobrazí skripty tak, jak jsou napsány.",
	  "Wyświetla skrypty tak, jak zostały napisane.", "Показывает скрипты в исходном виде.",
	  "Показує скрипти в оригінальному вигляді.", "Εμφανίζει τα σενάρια όπως είναι γραμμένα.", "Betikleri yazıldığı gibi gösterir.",
	  "Toont de scripts zoals ze geschreven zijn.", "Visar skripten som de är skrivna.", "Viser skriptene slik de er skrevet.",
	  "Näyttää skriptit sellaisina kuin ne on kirjoitettu.", "スクリプトを書かれたとおりに表示します。",
	  "스크립트를 작성된 그대로 표시합니다.", "按原样显示脚本。" },
	// P_OUTPUT
	{ "Langue affichée dans les scripts, en direct (fenêtres ouvertes comprises).",
	  "Language displayed in the scripts, live (open windows included).",
	  "In den Skripten angezeigte Sprache, sofort (auch geöffnete Fenster).",
	  "Idioma mostrado en los scripts, al instante (ventanas abiertas incluidas).",
	  "Idioma apresentado nos scripts, em direto (janelas abertas incluídas).",
	  "Lingua mostrata negli script, in tempo reale (finestre aperte comprese).",
	  "A szkriptekben megjelenő nyelv, azonnal (a nyitott ablakokban is).",
	  "Jazyk zobrazený ve skriptech, okamžitě (včetně otevřených oken).",
	  "Język wyświetlany w skryptach, na bieżąco (także w otwartych oknach).",
	  "Язык отображения скриптов, сразу (включая открытые окна).", "Мова відображення скриптів, одразу (разом із відкритими вікнами).",
	  "Γλώσσα εμφάνισης στα σενάρια, άμεσα (και στα ανοιχτά παράθυρα).", "Betiklerde gösterilen dil, anında (açık pencereler dahil).",
	  "Taal die in de scripts wordt getoond, direct (ook in open vensters).", "Språk som visas i skripten, direkt (även öppna fönster).",
	  "Språk som vises i skriptene, umiddelbart (også åpne vinduer).", "Skripteissä näytettävä kieli, heti (myös avoimet ikkunat).",
	  "スクリプトに表示する言語（開いているウィンドウにも即時反映）。", "스크립트에 표시되는 언어(열린 창에도 즉시 적용).",
	  "脚本中显示的语言（即时生效，包括已打开的窗口）。" },
	// P_TRANSLATOR
	{ "Traducteur des textes absents des dictionnaires (clé à saisir dans Clés des traducteurs).",
	  "Translator for texts not in the dictionaries (enter its key in Translator keys).",
	  "Übersetzer für Texte außerhalb der Wörterbücher (Schlüssel unter Übersetzer-Schlüssel eingeben).",
	  "Traductor para los textos fuera de los diccionarios (clave en Claves de los traductores).",
	  "Tradutor dos textos fora dos dicionários (chave em Chaves dos tradutores).",
	  "Traduttore per i testi non presenti nei dizionari (chiave in Chiavi dei traduttori).",
	  "Fordító a szótárakban nem szereplő szövegekhez (kulcs: Fordítói kulcsok).",
	  "Překladač textů mimo slovníky (klíč zadejte v Klíče překladačů).", "Tłumacz tekstów spoza słowników (klucz w Klucze tłumaczy).",
	  "Переводчик текстов вне словарей (ключ — в «Ключи переводчиков»).", "Перекладач текстів поза словниками (ключ — у «Ключі перекладачів»).",
	  "Μεταφραστής για κείμενα εκτός λεξικών (κλειδί στα Κλειδιά μεταφραστών).",
	  "Sözlükte olmayan metinler için çevirmen (anahtar: Çevirmen anahtarları).",
	  "Vertaler voor teksten buiten de woordenboeken (sleutel bij Vertaalsleutels).",
	  "Översättare för texter utanför ordlistorna (nyckel under Översättarnycklar).",
	  "Oversetter for tekster utenfor ordlistene (nøkkel under Oversetternøkler).",
	  "Kääntäjä sanastojen ulkopuolisille teksteille (avain: Kääntäjien avaimet).",
	  "辞書にないテキストの翻訳サービス（キーは「翻訳サービスのキー」で入力）。", "사전에 없는 텍스트용 번역기(키는 번역기 키에서 입력).",
	  "词典以外文本使用的翻译服务（在“翻译服务密钥”中输入密钥）。" },
	// P_KEYS
	{ "Ouvre cles.json : les clés restent sur ce poste, les scripts ne les voient jamais.",
	  "Opens cles.json: the keys stay on this computer, scripts never see them.",
	  "Öffnet cles.json: Die Schlüssel bleiben auf diesem Rechner, Skripte sehen sie nie.",
	  "Abre cles.json: las claves se quedan en este equipo, los scripts nunca las ven.",
	  "Abre cles.json: as chaves ficam neste computador, os scripts nunca as veem.",
	  "Apre cles.json: le chiavi restano su questo computer, gli script non le vedono mai.",
	  "Megnyitja a cles.json fájlt: a kulcsok ezen a gépen maradnak, a szkriptek sosem látják őket.",
	  "Otevře cles.json: klíče zůstávají v tomto počítači, skripty je nikdy nevidí.",
	  "Otwiera cles.json: klucze pozostają na tym komputerze, skrypty nigdy ich nie widzą.",
	  "Открывает cles.json: ключи остаются на этом компьютере, скрипты их не видят.",
	  "Відкриває cles.json: ключі лишаються на цьому комп’ютері, скрипти їх не бачать.",
	  "Ανοίγει το cles.json: τα κλειδιά μένουν σε αυτόν τον υπολογιστή, τα σενάρια δεν τα βλέπουν ποτέ.",
	  "cles.json dosyasını açar: anahtarlar bu bilgisayarda kalır, betikler onları asla görmez.",
	  "Opent cles.json: de sleutels blijven op deze computer, scripts zien ze nooit.",
	  "Öppnar cles.json: nycklarna stannar på den här datorn, skripten ser dem aldrig.",
	  "Åpner cles.json: nøklene blir på denne datamaskinen, skriptene ser dem aldri.",
	  "Avaa cles.json-tiedoston: avaimet pysyvät tällä koneella, skriptit eivät näe niitä.",
	  "cles.json を開きます。キーはこのコンピューターに保存され、スクリプトからは見えません。",
	  "cles.json을 엽니다. 키는 이 컴퓨터에만 저장되며 스크립트는 볼 수 없습니다.", "打开 cles.json：密钥保存在本机，脚本永远看不到。" },
	// P_CORRECTIONS
	{ "Ouvre corrections.json : termes à corriger dans les traductions en ligne, appliqués aussitôt.",
	  "Opens corrections.json: terms to fix in online translations, applied immediately.",
	  "Öffnet corrections.json: in Online-Übersetzungen zu korrigierende Begriffe, sofort angewendet.",
	  "Abre corrections.json: términos que corregir en las traducciones en línea, aplicados al instante.",
	  "Abre corrections.json: termos a corrigir nas traduções online, aplicados de imediato.",
	  "Apre corrections.json: termini da correggere nelle traduzioni online, applicati subito.",
	  "Megnyitja a corrections.json fájlt: az online fordításokban javítandó kifejezések, azonnal érvényesek.",
	  "Otevře corrections.json: termíny k opravě v online překladech, použijí se okamžitě.",
	  "Otwiera corrections.json: terminy do poprawienia w tłumaczeniach online, stosowane od razu.",
	  "Открывает corrections.json: термины для исправления в онлайн-переводах, применяются сразу.",
	  "Відкриває corrections.json: терміни для виправлення в онлайн-перекладах, застосовуються одразу.",
	  "Ανοίγει το corrections.json: όροι προς διόρθωση στις διαδικτυακές μεταφράσεις, εφαρμόζονται αμέσως.",
	  "corrections.json dosyasını açar: çevrimiçi çevirilerde düzeltilecek terimler, hemen uygulanır.",
	  "Opent corrections.json: te corrigeren termen in online vertalingen, direct toegepast.",
	  "Öppnar corrections.json: termer att rätta i onlineöversättningar, tillämpas direkt.",
	  "Åpner corrections.json: termer som skal rettes i nettbaserte oversettelser, brukes straks.",
	  "Avaa corrections.json-tiedoston: verkkokäännöksissä korjattavat termit, käytössä heti.",
	  "corrections.json を開きます。オンライン翻訳で修正する用語で、すぐに反映されます。",
	  "corrections.json을 엽니다. 온라인 번역에서 고칠 용어이며 즉시 적용됩니다.", "打开 corrections.json：需在在线翻译中更正的术语，立即生效。" },
	// P_ONLINE
	{ "Les textes absents des dictionnaires sont traduits en ligne puis gardés en cache sur ce poste.",
	  "Texts not in the dictionaries are translated online, then cached on this computer.",
	  "Texte außerhalb der Wörterbücher werden online übersetzt und auf diesem Rechner zwischengespeichert.",
	  "Los textos fuera de los diccionarios se traducen en línea y se guardan en caché en este equipo.",
	  "Os textos fora dos dicionários são traduzidos online e guardados em cache neste computador.",
	  "I testi non presenti nei dizionari sono tradotti online e conservati nella cache di questo computer.",
	  "A szótárakban nem szereplő szövegek online fordítása, majd tárolása ezen a gépen.",
	  "Texty mimo slovníky se přeloží online a uloží do mezipaměti v tomto počítači.",
	  "Teksty spoza słowników są tłumaczone online i zapisywane w pamięci podręcznej tego komputera.",
	  "Тексты вне словарей переводятся онлайн и сохраняются в кэше на этом компьютере.",
	  "Тексти поза словниками перекладаються онлайн і зберігаються в кеші на цьому комп’ютері.",
	  "Τα κείμενα εκτός λεξικών μεταφράζονται διαδικτυακά και αποθηκεύονται σε αυτόν τον υπολογιστή.",
	  "Sözlükte olmayan metinler çevrimiçi çevrilir ve bu bilgisayarda önbelleğe alınır.",
	  "Teksten buiten de woordenboeken worden online vertaald en op deze computer bewaard.",
	  "Texter utanför ordlistorna översätts online och sparas i cache på den här datorn.",
	  "Tekster utenfor ordlistene oversettes på nett og lagres på denne datamaskinen.",
	  "Sanastojen ulkopuoliset tekstit käännetään verkossa ja tallennetaan tälle koneelle.",
	  "辞書にないテキストはオンラインで翻訳され、このコンピューターにキャッシュされます。",
	  "사전에 없는 텍스트는 온라인으로 번역된 후 이 컴퓨터에 캐시됩니다.", "词典以外的文本在线翻译后缓存在本机。" },
	// P_COLLECT
	{ "Note les textes sans traduction dans le dossier manquants.", "Records texts without translation in the manquants folder.",
	  "Notiert Texte ohne Übersetzung im Ordner manquants.", "Anota los textos sin traducción en la carpeta manquants.",
	  "Regista os textos sem tradução na pasta manquants.", "Annota i testi senza traduzione nella cartella manquants.",
	  "A fordítás nélküli szövegeket a manquants mappába jegyzi.", "Zapisuje texty bez překladu do složky manquants.",
	  "Zapisuje teksty bez tłumaczenia w folderze manquants.", "Записывает тексты без перевода в папку manquants.",
	  "Записує тексти без перекладу в папку manquants.", "Καταγράφει τα κείμενα χωρίς μετάφραση στον φάκελο manquants.",
	  "Çevirisi olmayan metinleri manquants klasörüne kaydeder.", "Noteert teksten zonder vertaling in de map manquants.",
	  "Antecknar texter utan översättning i mappen manquants.", "Noterer tekster uten oversettelse i mappen manquants.",
	  "Kirjaa käännöksettömät tekstit manquants-kansioon.", "未翻訳のテキストを manquants フォルダーに記録します。",
	  "번역이 없는 텍스트를 manquants 폴더에 기록합니다.", "将没有翻译的文本记录到 manquants 文件夹。" },
	// P_FOLDER
	{ "Ouvre le dossier de Tapir Translator (dictionnaires, cache, journal).",
	  "Opens the Tapir Translator folder (dictionaries, cache, log).",
	  "Öffnet den Ordner von Tapir Translator (Wörterbücher, Cache, Protokoll).",
	  "Abre la carpeta de Tapir Translator (diccionarios, caché, registro).",
	  "Abre a pasta do Tapir Translator (dicionários, cache, registo).", "Apre la cartella di Tapir Translator (dizionari, cache, registro).",
	  "Megnyitja a Tapir Translator mappáját (szótárak, gyorsítótár, napló).",
	  "Otevře složku Tapir Translator (slovníky, mezipaměť, protokol).",
	  "Otwiera folder Tapir Translator (słowniki, pamięć podręczna, dziennik).", "Открывает папку Tapir Translator (словари, кэш, журнал).",
	  "Відкриває папку Tapir Translator (словники, кеш, журнал).",
	  "Ανοίγει τον φάκελο του Tapir Translator (λεξικά, προσωρινή μνήμη, αρχείο καταγραφής).",
	  "Tapir Translator klasörünü açar (sözlükler, önbellek, günlük).",
	  "Opent de map van Tapir Translator (woordenboeken, cache, logboek).",
	  "Öppnar mappen för Tapir Translator (ordlistor, cache, logg).", "Åpner mappen til Tapir Translator (ordlister, hurtigbuffer, logg).",
	  "Avaa Tapir Translatorin kansion (sanastot, välimuisti, loki).", "Tapir Translator のフォルダーを開きます（辞書、キャッシュ、ログ）。",
	  "Tapir Translator 폴더를 엽니다(사전, 캐시, 로그).", "打开 Tapir Translator 文件夹（词典、缓存、日志）。" },
	// P_STATUS
	{ "Écrit l'état complet de Tapir Translator dans le Rapport de session.", "Writes the full Tapir Translator status to the Session Report.",
	  "Schreibt den vollständigen Status in den Sitzungsbericht.", "Escribe el estado completo en el Informe de sesión.",
	  "Escreve o estado completo no Relatório da sessão.", "Scrive lo stato completo nel Rapporto di sessione.",
	  "A teljes állapotot a munkamenet-jelentésbe írja.", "Zapíše úplný stav do Zprávy relace.", "Zapisuje pełny stan w Raporcie sesji.",
	  "Записывает полное состояние в отчёт сеанса.", "Записує повний стан у звіт сеансу.",
	  "Γράφει την πλήρη κατάσταση στην Αναφορά συνεδρίας.", "Tam durumu Oturum Raporuna yazar.",
	  "Schrijft de volledige status naar het sessierapport.", "Skriver hela statusen till sessionsrapporten.",
	  "Skriver full status til øktrapporten.", "Kirjoittaa koko tilan istuntoraporttiin.", "状態の詳細をセッションレポートに書き込みます。",
	  "전체 상태를 세션 보고서에 기록합니다.", "将完整状态写入会话报告。" },
	// P_INTERFACE
	{ "Langue des menus et messages de Tapir Translator (menus : au prochain démarrage d'ArchiCAD).",
	  "Language of the Tapir Translator menus and messages (menus: at the next ArchiCAD start).",
	  "Sprache der Menüs und Meldungen (Menüs: beim nächsten Start von ArchiCAD).",
	  "Idioma de los menús y mensajes (menús: al próximo inicio de ArchiCAD).",
	  "Idioma dos menus e mensagens (menus: no próximo arranque do ArchiCAD).",
	  "Lingua di menu e messaggi (menu: al prossimo avvio di ArchiCAD).",
	  "A menük és üzenetek nyelve (menük: az ArchiCAD következő indításakor).",
	  "Jazyk nabídek a zpráv (nabídky: při příštím spuštění ArchiCADu).",
	  "Język menu i komunikatów (menu: przy następnym uruchomieniu ArchiCADa).",
	  "Язык меню и сообщений (меню: при следующем запуске ArchiCAD).", "Мова меню й повідомлень (меню: під час наступного запуску ArchiCAD).",
	  "Γλώσσα μενού και μηνυμάτων (μενού: στην επόμενη εκκίνηση του ArchiCAD).",
	  "Menü ve mesaj dili (menüler: ArchiCAD bir sonraki açılışta).",
	  "Taal van menu's en meldingen (menu's: bij de volgende start van ArchiCAD).",
	  "Språk för menyer och meddelanden (menyer: vid nästa start av ArchiCAD).",
	  "Språk for menyer og meldinger (menyer: ved neste oppstart av ArchiCAD).",
	  "Valikkojen ja viestien kieli (valikot: ArchiCADin seuraavassa käynnistyksessä).",
	  "メニューとメッセージの言語（メニューは次回の ArchiCAD 起動時に反映）。", "메뉴와 메시지 언어(메뉴는 다음 ArchiCAD 시작 시 적용).",
	  "菜单和消息的语言（菜单在下次启动 ArchiCAD 时生效）。" },

	// D_TITLE
	{ "quota MyMemory atteint", "MyMemory quota reached", "MyMemory-Kontingent erreicht", "Cuota de MyMemory alcanzada",
	  "Quota do MyMemory atingida", "Quota MyMemory raggiunta", "Elérte a MyMemory keretét", "Kvóta MyMemory vyčerpána",
	  "Wyczerpano limit MyMemory", "Квота MyMemory исчерпана", "Квоту MyMemory вичерпано", "Εξαντλήθηκε το όριο του MyMemory",
	  "MyMemory kotası doldu", "MyMemory-quotum bereikt", "MyMemory-kvoten är nådd", "MyMemory-kvoten er nådd",
	  "MyMemory-kiintiö täynnä", "MyMemory の上限に達しました", "MyMemory 한도 도달", "已达到 MyMemory 配额" },
	// D_ANONYMOUS
	{ "MyMemory a atteint son quota gratuit anonyme (environ 5 000 caractères par jour).\r\n\r\nAvec une adresse e-mail, MyMemory accorde environ 50 000 caractères par jour. Aucun compte ni mot de passe n'est nécessaire. L'adresse est envoyée à MyMemory avec chaque demande et enregistrée dans cles.json sur ce poste.",
	  "MyMemory has reached its free anonymous quota (about 5,000 characters per day).\r\n\r\nWith an e-mail address, MyMemory allows about 50,000 characters per day. No account or password is needed. The address is sent to MyMemory with each request and saved in cles.json on this computer.",
	  "MyMemory hat sein kostenloses anonymes Kontingent erreicht (etwa 5.000 Zeichen pro Tag).\r\n\r\nMit einer E-Mail-Adresse erlaubt MyMemory etwa 50.000 Zeichen pro Tag. Weder Konto noch Passwort sind nötig. Die Adresse wird mit jeder Anfrage an MyMemory gesendet und in cles.json auf diesem Rechner gespeichert.",
	  "MyMemory ha alcanzado su cuota gratuita anónima (unos 5000 caracteres al día).\r\n\r\nCon una dirección de correo, MyMemory permite unos 50 000 caracteres al día. No hace falta cuenta ni contraseña. La dirección se envía a MyMemory con cada petición y se guarda en cles.json en este equipo.",
	  "O MyMemory atingiu a sua quota gratuita anónima (cerca de 5000 caracteres por dia).\r\n\r\nCom um endereço de e-mail, o MyMemory permite cerca de 50 000 caracteres por dia. Não é preciso conta nem palavra-passe. O endereço é enviado ao MyMemory em cada pedido e guardado em cles.json neste computador.",
	  "MyMemory ha raggiunto la quota gratuita anonima (circa 5.000 caratteri al giorno).\r\n\r\nCon un indirizzo e-mail, MyMemory consente circa 50.000 caratteri al giorno. Non servono account né password. L'indirizzo viene inviato a MyMemory con ogni richiesta e salvato in cles.json su questo computer.",
	  "A MyMemory elérte az ingyenes névtelen keretét (kb. 5000 karakter naponta).\r\n\r\nE-mail-címmel a MyMemory naponta kb. 50 000 karaktert engedélyez. Nincs szükség fiókra vagy jelszóra. A cím minden kéréssel a MyMemoryhoz kerül, és ezen a gépen a cles.json fájlba mentődik.",
	  "MyMemory vyčerpal bezplatnou anonymní kvótu (asi 5 000 znaků denně).\r\n\r\nS e-mailovou adresou povoluje MyMemory asi 50 000 znaků denně. Účet ani heslo nejsou potřeba. Adresa se odesílá MyMemory s každým požadavkem a ukládá se do cles.json v tomto počítači.",
	  "MyMemory wyczerpał bezpłatny limit anonimowy (ok. 5000 znaków dziennie).\r\n\r\nZ adresem e-mail MyMemory pozwala na ok. 50 000 znaków dziennie. Konto ani hasło nie są potrzebne. Adres jest wysyłany do MyMemory z każdym zapytaniem i zapisywany w cles.json na tym komputerze.",
	  "MyMemory исчерпал бесплатную анонимную квоту (около 5000 символов в день).\r\n\r\nС адресом электронной почты MyMemory разрешает около 50 000 символов в день. Учётная запись и пароль не нужны. Адрес отправляется в MyMemory с каждым запросом и сохраняется в cles.json на этом компьютере.",
	  "MyMemory вичерпав безкоштовну анонімну квоту (близько 5000 символів на день).\r\n\r\nЗ адресою електронної пошти MyMemory дозволяє близько 50 000 символів на день. Обліковий запис і пароль не потрібні. Адреса надсилається до MyMemory з кожним запитом і зберігається в cles.json на цьому комп’ютері.",
	  "Το MyMemory εξάντλησε το δωρεάν ανώνυμο όριο (περίπου 5.000 χαρακτήρες την ημέρα).\r\n\r\nΜε μια διεύθυνση email, το MyMemory επιτρέπει περίπου 50.000 χαρακτήρες την ημέρα. Δεν χρειάζεται λογαριασμός ή κωδικός. Η διεύθυνση στέλνεται στο MyMemory με κάθε αίτημα και αποθηκεύεται στο cles.json σε αυτόν τον υπολογιστή.",
	  "MyMemory ücretsiz anonim kotasını doldurdu (günde yaklaşık 5.000 karakter).\r\n\r\nBir e-posta adresiyle MyMemory günde yaklaşık 50.000 karaktere izin verir. Hesap veya parola gerekmez. Adres her istekle MyMemory'ye gönderilir ve bu bilgisayarda cles.json dosyasına kaydedilir.",
	  "MyMemory heeft zijn gratis anonieme quotum bereikt (ongeveer 5.000 tekens per dag).\r\n\r\nMet een e-mailadres staat MyMemory ongeveer 50.000 tekens per dag toe. Een account of wachtwoord is niet nodig. Het adres wordt bij elke aanvraag naar MyMemory gestuurd en in cles.json op deze computer opgeslagen.",
	  "MyMemory har nått sin kostnadsfria anonyma kvot (cirka 5 000 tecken per dag).\r\n\r\nMed en e-postadress tillåter MyMemory cirka 50 000 tecken per dag. Inget konto eller lösenord behövs. Adressen skickas till MyMemory med varje förfrågan och sparas i cles.json på den här datorn.",
	  "MyMemory har nådd den gratis anonyme kvoten (omtrent 5 000 tegn per dag).\r\n\r\nMed en e-postadresse tillater MyMemory omtrent 50 000 tegn per dag. Konto eller passord trengs ikke. Adressen sendes til MyMemory med hver forespørsel og lagres i cles.json på denne datamaskinen.",
	  "MyMemory on käyttänyt ilmaisen anonyymin kiintiönsä (noin 5 000 merkkiä päivässä).\r\n\r\nSähköpostiosoitteella MyMemory sallii noin 50 000 merkkiä päivässä. Tiliä tai salasanaa ei tarvita. Osoite lähetetään MyMemorylle jokaisen pyynnön mukana ja tallennetaan tämän koneen cles.json-tiedostoon.",
	  "MyMemory の匿名の無料上限（1 日約 5,000 文字）に達しました。\r\n\r\nメールアドレスを登録すると、MyMemory は 1 日約 50,000 文字まで使えます。アカウントやパスワードは不要です。アドレスは毎回の要求とともに MyMemory に送信され、このコンピューターの cles.json に保存されます。",
	  "MyMemory의 무료 익명 한도(하루 약 5,000자)에 도달했습니다.\r\n\r\n이메일 주소를 입력하면 MyMemory에서 하루 약 50,000자를 사용할 수 있습니다. 계정이나 비밀번호는 필요 없습니다. 주소는 요청할 때마다 MyMemory로 전송되며 이 컴퓨터의 cles.json에 저장됩니다.",
	  "MyMemory 的匿名免费配额（每天约 5,000 个字符）已用完。\r\n\r\n提供电子邮件地址后，MyMemory 每天允许约 50,000 个字符。无需账户或密码。该地址会随每次请求发送给 MyMemory，并保存在本机的 cles.json 中。" },
	// D_WITH_EMAIL
	{ "MyMemory a atteint aujourd'hui son quota gratuit, même avec votre adresse e-mail (environ 50 000 caractères par jour).\r\n\r\nLes textes déjà traduits restent affichés depuis le cache. Pour le reste : attendez demain, ou choisissez un autre traducteur dans ArchiCAD, menu {menu}.",
	  "MyMemory has reached today's free quota, even with your e-mail address (about 50,000 characters per day).\r\n\r\nTexts already translated are still shown from the cache. For the rest: wait until tomorrow, or choose another translator in ArchiCAD, menu {menu}.",
	  "MyMemory hat das heutige kostenlose Kontingent erreicht, auch mit Ihrer E-Mail-Adresse (etwa 50.000 Zeichen pro Tag).\r\n\r\nBereits übersetzte Texte werden weiter aus dem Cache angezeigt. Für den Rest: bis morgen warten oder in ArchiCAD einen anderen Übersetzer wählen, Menü {menu}.",
	  "MyMemory ha alcanzado hoy su cuota gratuita, incluso con su dirección de correo (unos 50 000 caracteres al día).\r\n\r\nLos textos ya traducidos siguen mostrándose desde la caché. Para el resto: espere a mañana o elija otro traductor en ArchiCAD, menú {menu}.",
	  "O MyMemory atingiu hoje a quota gratuita, mesmo com o seu e-mail (cerca de 50 000 caracteres por dia).\r\n\r\nOs textos já traduzidos continuam a ser mostrados a partir da cache. Para o resto: aguarde até amanhã ou escolha outro tradutor no ArchiCAD, menu {menu}.",
	  "MyMemory ha raggiunto la quota gratuita di oggi, anche con il suo indirizzo e-mail (circa 50.000 caratteri al giorno).\r\n\r\nI testi già tradotti restano visibili dalla cache. Per il resto: attenda domani o scelga un altro traduttore in ArchiCAD, menu {menu}.",
	  "A MyMemory mára elérte az ingyenes keretet, az e-mail-címmel együtt is (kb. 50 000 karakter naponta).\r\n\r\nA már lefordított szövegek továbbra is a gyorsítótárból jelennek meg. A többihez várjon holnapig, vagy válasszon másik fordítót az ArchiCAD-ben: {menu}.",
	  "MyMemory dnes vyčerpal bezplatnou kvótu i s vaší e-mailovou adresou (asi 50 000 znaků denně).\r\n\r\nUž přeložené texty se dál zobrazují z mezipaměti. Pro zbytek počkejte do zítřka nebo v ArchiCADu zvolte jiný překladač, nabídka {menu}.",
	  "MyMemory wyczerpał dzisiejszy bezpłatny limit, nawet z Twoim adresem e-mail (ok. 50 000 znaków dziennie).\r\n\r\nPrzetłumaczone już teksty są nadal wyświetlane z pamięci podręcznej. Dla pozostałych: poczekaj do jutra lub wybierz innego tłumacza w ArchiCADzie, menu {menu}.",
	  "MyMemory исчерпал сегодняшнюю бесплатную квоту даже с вашим адресом почты (около 50 000 символов в день).\r\n\r\nУже переведённые тексты по-прежнему показываются из кэша. Для остального подождите до завтра или выберите другого переводчика в ArchiCAD, меню {menu}.",
	  "MyMemory вичерпав сьогоднішню безкоштовну квоту навіть із вашою адресою пошти (близько 50 000 символів на день).\r\n\r\nУже перекладені тексти й далі показуються з кешу. Для решти зачекайте до завтра або виберіть іншого перекладача в ArchiCAD, меню {menu}.",
	  "Το MyMemory εξάντλησε το σημερινό δωρεάν όριο, ακόμη και με το email σας (περίπου 50.000 χαρακτήρες την ημέρα).\r\n\r\nΤα κείμενα που έχουν ήδη μεταφραστεί εμφανίζονται από την προσωρινή μνήμη. Για τα υπόλοιπα: περιμένετε ως αύριο ή επιλέξτε άλλο μεταφραστή στο ArchiCAD, μενού {menu}.",
	  "MyMemory, e-posta adresinizle bile bugünkü ücretsiz kotayı doldurdu (günde yaklaşık 50.000 karakter).\r\n\r\nÇevrilmiş metinler önbellekten gösterilmeye devam eder. Geri kalanı için yarını bekleyin veya ArchiCAD'de başka bir çevirmen seçin: {menu}.",
	  "MyMemory heeft het gratis quotum van vandaag bereikt, ook met uw e-mailadres (ongeveer 50.000 tekens per dag).\r\n\r\nAl vertaalde teksten worden nog uit de cache getoond. Voor de rest: wacht tot morgen of kies een andere vertaler in ArchiCAD, menu {menu}.",
	  "MyMemory har nått dagens kostnadsfria kvot, även med din e-postadress (cirka 50 000 tecken per dag).\r\n\r\nRedan översatta texter visas fortfarande från cachen. För resten: vänta till i morgon eller välj en annan översättare i ArchiCAD, menyn {menu}.",
	  "MyMemory har nådd dagens gratiskvote, også med e-postadressen din (omtrent 50 000 tegn per dag).\r\n\r\nTekster som alt er oversatt, vises fortsatt fra hurtigbufferen. For resten: vent til i morgen, eller velg en annen oversetter i ArchiCAD, menyen {menu}.",
	  "MyMemory on käyttänyt tämän päivän ilmaisen kiintiön myös sähköpostiosoitteellasi (noin 50 000 merkkiä päivässä).\r\n\r\nJo käännetyt tekstit näytetään edelleen välimuistista. Muille: odota huomiseen tai valitse ArchiCADissa toinen kääntäjä, valikko {menu}.",
	  "メールアドレスを登録していても、本日の MyMemory の無料上限（1 日約 50,000 文字）に達しました。\r\n\r\n翻訳済みのテキストはキャッシュから引き続き表示されます。残りは明日までお待ちいただくか、ArchiCAD のメニュー {menu} で別の翻訳サービスを選択してください。",
	  "이메일 주소를 입력했어도 오늘의 MyMemory 무료 한도(하루 약 50,000자)에 도달했습니다.\r\n\r\n이미 번역된 텍스트는 캐시에서 계속 표시됩니다. 나머지는 내일까지 기다리거나 ArchiCAD의 {menu} 메뉴에서 다른 번역기를 선택하십시오.",
	  "即使提供了电子邮件地址，今天的 MyMemory 免费配额（每天约 50,000 个字符）也已用完。\r\n\r\n已翻译的文本仍会从缓存中显示。其余文本：请等到明天，或在 ArchiCAD 的 {menu} 菜单中选择其他翻译服务。" },
	// D_LABEL
	{ "Adresse e-mail :", "E-mail address:", "E-Mail-Adresse:", "Correo electrónico:", "Endereço de e-mail:", "Indirizzo e-mail:",
	  "E-mail-cím:", "E-mailová adresa:", "Adres e-mail:", "Адрес эл. почты:", "Адреса ел. пошти:", "Διεύθυνση email:",
	  "E-posta adresi:", "E-mailadres:", "E-postadress:", "E-postadresse:", "Sähköpostiosoite:", "メールアドレス:", "이메일 주소:",
	  "电子邮件地址：" },
	// D_SAVE
	{ "Enregistrer", "Save", "Speichern", "Guardar", "Guardar", "Salva", "Mentés", "Uložit", "Zapisz", "Сохранить", "Зберегти",
	  "Αποθήκευση", "Kaydet", "Opslaan", "Spara", "Lagre", "Tallenna", "保存", "저장", "保存" },
	// D_LATER
	{ "Plus tard", "Later", "Später", "Más tarde", "Mais tarde", "Più tardi", "Később", "Později", "Później", "Позже", "Пізніше",
	  "Αργότερα", "Daha sonra", "Later", "Senare", "Senere", "Myöhemmin", "後で", "나중에", "稍后" },
	// D_INVALID
	{ "Adresse e-mail invalide.", "Invalid e-mail address.", "Ungültige E-Mail-Adresse.", "Dirección de correo no válida.",
	  "Endereço de e-mail inválido.", "Indirizzo e-mail non valido.", "Érvénytelen e-mail-cím.", "Neplatná e-mailová adresa.",
	  "Nieprawidłowy adres e-mail.", "Неверный адрес эл. почты.", "Неправильна адреса ел. пошти.", "Μη έγκυρη διεύθυνση email.",
	  "Geçersiz e-posta adresi.", "Ongeldig e-mailadres.", "Ogiltig e-postadress.", "Ugyldig e-postadresse.",
	  "Virheellinen sähköpostiosoite.", "メールアドレスが正しくありません。", "이메일 주소가 올바르지 않습니다.", "电子邮件地址无效。" },

	// R_STARTUP
	{ "Tapir Translator {v} actif. Langues : {l}.", "Tapir Translator {v} active. Languages: {l}.",
	  "Tapir Translator {v} aktiv. Sprachen: {l}.", "Tapir Translator {v} activo. Idiomas: {l}.", "Tapir Translator {v} ativo. Idiomas: {l}.",
	  "Tapir Translator {v} attivo. Lingue: {l}.", "Tapir Translator {v} aktív. Nyelvek: {l}.", "Tapir Translator {v} je aktivní. Jazyky: {l}.",
	  "Tapir Translator {v} aktywny. Języki: {l}.", "Tapir Translator {v} активен. Языки: {l}.", "Tapir Translator {v} активний. Мови: {l}.",
	  "Tapir Translator {v} ενεργό. Γλώσσες: {l}.", "Tapir Translator {v} etkin. Diller: {l}.", "Tapir Translator {v} actief. Talen: {l}.",
	  "Tapir Translator {v} aktiv. Språk: {l}.", "Tapir Translator {v} aktiv. Språk: {l}.", "Tapir Translator {v} käytössä. Kielet: {l}.",
	  "Tapir Translator {v} 有効。言語: {l}", "Tapir Translator {v} 활성. 언어: {l}", "Tapir Translator {v} 已启用。语言：{l}" },
	// R_INACTIVE
	{ "Tapir Translator {v} INACTIF : voir les erreurs ci-dessus.", "Tapir Translator {v} INACTIVE: see the errors above.",
	  "Tapir Translator {v} INAKTIV: siehe Fehler oben.", "Tapir Translator {v} INACTIVO: vea los errores anteriores.",
	  "Tapir Translator {v} INATIVO: veja os erros acima.", "Tapir Translator {v} NON ATTIVO: vedere gli errori sopra.",
	  "Tapir Translator {v} INAKTÍV: lásd a fenti hibákat.", "Tapir Translator {v} NEAKTIVNÍ: viz chyby výše.",
	  "Tapir Translator {v} NIEAKTYWNY: zobacz błędy powyżej.", "Tapir Translator {v} НЕАКТИВЕН: см. ошибки выше.",
	  "Tapir Translator {v} НЕАКТИВНИЙ: див. помилки вище.", "Tapir Translator {v} ΑΝΕΝΕΡΓΟ: δείτε τα σφάλματα παραπάνω.",
	  "Tapir Translator {v} DEVRE DIŞI: yukarıdaki hatalara bakın.", "Tapir Translator {v} NIET ACTIEF: zie de fouten hierboven.",
	  "Tapir Translator {v} INAKTIV: se felen ovan.", "Tapir Translator {v} INAKTIV: se feilene over.",
	  "Tapir Translator {v} EI KÄYTÖSSÄ: katso virheet yllä.", "Tapir Translator {v} は無効です。上のエラーを確認してください。",
	  "Tapir Translator {v} 비활성: 위의 오류를 확인하십시오.", "Tapir Translator {v} 未启用：请查看上方的错误。" },
	// R_SETTINGS
	{ "Réglages enregistrés : {l} ; traduction en ligne : {o}. Les scripts ouverts suivent aussitôt.",
	  "Settings saved: {l}; online translation: {o}. Open scripts follow immediately.",
	  "Einstellungen gespeichert: {l}; Online-Übersetzung: {o}. Geöffnete Skripte folgen sofort.",
	  "Ajustes guardados: {l}; traducción en línea: {o}. Los scripts abiertos se actualizan al instante.",
	  "Definições guardadas: {l}; tradução online: {o}. Os scripts abertos acompanham de imediato.",
	  "Impostazioni salvate: {l}; traduzione online: {o}. Gli script aperti si aggiornano subito.",
	  "Beállítások mentve: {l}; online fordítás: {o}. A nyitott szkriptek azonnal követik.",
	  "Nastavení uloženo: {l}; online překlad: {o}. Otevřené skripty se hned přizpůsobí.",
	  "Ustawienia zapisane: {l}; tłumaczenie online: {o}. Otwarte skrypty dostosują się od razu.",
	  "Настройки сохранены: {l}; онлайн-перевод: {o}. Открытые скрипты обновятся сразу.",
	  "Налаштування збережено: {l}; онлайн-переклад: {o}. Відкриті скрипти оновляться одразу.",
	  "Οι ρυθμίσεις αποθηκεύτηκαν: {l}· διαδικτυακή μετάφραση: {o}. Τα ανοιχτά σενάρια ενημερώνονται αμέσως.",
	  "Ayarlar kaydedildi: {l}; çevrimiçi çeviri: {o}. Açık betikler hemen güncellenir.",
	  "Instellingen opgeslagen: {l}; online vertaling: {o}. Open scripts volgen direct.",
	  "Inställningar sparade: {l}; onlineöversättning: {o}. Öppna skript följer direkt.",
	  "Innstillinger lagret: {l}; nettbasert oversettelse: {o}. Åpne skript følger straks.",
	  "Asetukset tallennettu: {l}; verkkokäännös: {o}. Avoimet skriptit päivittyvät heti.",
	  "設定を保存しました: {l}、オンライン翻訳: {o}。開いているスクリプトにすぐ反映されます。",
	  "설정 저장됨: {l}, 온라인 번역: {o}. 열린 스크립트에 즉시 반영됩니다.", "设置已保存：{l}；在线翻译：{o}。已打开的脚本立即更新。" },
	// R_SAME_LANGUAGE
	{ "Langue d'entrée = langue de sortie : rien ne sera traduit.", "Input language = output language: nothing will be translated.",
	  "Eingabesprache = Ausgabesprache: Es wird nichts übersetzt.", "Idioma de entrada = idioma de salida: no se traducirá nada.",
	  "Idioma de entrada = idioma de saída: nada será traduzido.", "Lingua di origine = lingua di destinazione: non verrà tradotto nulla.",
	  "Bemeneti nyelv = kimeneti nyelv: nem lesz fordítás.", "Vstupní jazyk = výstupní jazyk: nic se nepřeloží.",
	  "Język wejściowy = język wyjściowy: nic nie zostanie przetłumaczone.", "Исходный язык совпадает с языком перевода: перевода не будет.",
	  "Мова оригіналу збігається з мовою перекладу: перекладу не буде.", "Γλώσσα εισόδου = γλώσσα εξόδου: δεν θα μεταφραστεί τίποτα.",
	  "Giriş dili = çıkış dili: hiçbir şey çevrilmeyecek.", "Brontaal = doeltaal: er wordt niets vertaald.",
	  "Källspråk = målspråk: inget översätts.", "Kildespråk = målspråk: ingenting blir oversatt.",
	  "Lähdekieli = kohdekieli: mitään ei käännetä.", "入力言語と出力言語が同じため、翻訳されません。",
	  "입력 언어와 출력 언어가 같아 번역되지 않습니다.", "输入语言与输出语言相同：不会翻译任何内容。" },
	// R_YES
	{ "oui", "yes", "ja", "sí", "sim", "sì", "igen", "ano", "tak", "да", "так", "ναι", "evet", "ja", "ja", "ja", "kyllä", "はい", "예", "是" },
	// R_NO
	{ "non", "no", "nein", "no", "não", "no", "nem", "ne", "nie", "нет", "ні", "όχι", "hayır", "nee", "nej", "nei", "ei", "いいえ", "아니요", "否" },
	// R_ENGINE_STARTED
	{ "Traduction en ligne active : {f} ({e}).", "Online translation active: {f} ({e}).", "Online-Übersetzung aktiv: {f} ({e}).",
	  "Traducción en línea activa: {f} ({e}).", "Tradução online ativa: {f} ({e}).", "Traduzione online attiva: {f} ({e}).",
	  "Online fordítás aktív: {f} ({e}).", "Online překlad je aktivní: {f} ({e}).", "Tłumaczenie online aktywne: {f} ({e}).",
	  "Онлайн-перевод включён: {f} ({e}).", "Онлайн-переклад увімкнено: {f} ({e}).", "Η διαδικτυακή μετάφραση είναι ενεργή: {f} ({e}).",
	  "Çevrimiçi çeviri etkin: {f} ({e}).", "Online vertaling actief: {f} ({e}).", "Onlineöversättning aktiv: {f} ({e}).",
	  "Nettbasert oversettelse aktiv: {f} ({e}).", "Verkkokäännös käytössä: {f} ({e}).", "オンライン翻訳を有効にしました: {f}（{e}）。",
	  "온라인 번역 활성: {f}({e}).", "在线翻译已启用：{f}（{e}）。" },
	// R_ENGINE_STOPPED
	{ "Traduction en ligne arrêtée.", "Online translation stopped.", "Online-Übersetzung beendet.", "Traducción en línea detenida.",
	  "Tradução online parada.", "Traduzione online interrotta.", "Online fordítás leállítva.", "Online překlad zastaven.",
	  "Tłumaczenie online zatrzymane.", "Онлайн-перевод остановлен.", "Онлайн-переклад зупинено.", "Η διαδικτυακή μετάφραση σταμάτησε.",
	  "Çevrimiçi çeviri durduruldu.", "Online vertaling gestopt.", "Onlineöversättning stoppad.", "Nettbasert oversettelse stoppet.",
	  "Verkkokäännös pysäytetty.", "オンライン翻訳を停止しました。", "온라인 번역이 중지되었습니다.", "在线翻译已停止。" },
	// R_ENGINE_NOT_FOUND
	{ "ERREUR : moteur de traduction introuvable ; les textes hors dictionnaire restent dans leur langue.",
	  "ERROR: translation engine not found; texts not in the dictionaries stay in their language.",
	  "FEHLER: Übersetzungsmodul nicht gefunden; Texte außerhalb der Wörterbücher bleiben unübersetzt.",
	  "ERROR: motor de traducción no encontrado; los textos fuera de los diccionarios quedan sin traducir.",
	  "ERRO: motor de tradução não encontrado; os textos fora dos dicionários ficam por traduzir.",
	  "ERRORE: motore di traduzione non trovato; i testi non presenti nei dizionari restano non tradotti.",
	  "HIBA: a fordítómotor nem található; a szótáron kívüli szövegek fordítatlanok maradnak.",
	  "CHYBA: překladový modul nenalezen; texty mimo slovníky zůstanou nepřeložené.",
	  "BŁĄD: nie znaleziono modułu tłumaczenia; teksty spoza słowników pozostaną nieprzetłumaczone.",
	  "ОШИБКА: модуль перевода не найден; тексты вне словарей останутся без перевода.",
	  "ПОМИЛКА: модуль перекладу не знайдено; тексти поза словниками залишаться без перекладу.",
	  "ΣΦΑΛΜΑ: δεν βρέθηκε η μηχανή μετάφρασης· τα κείμενα εκτός λεξικών μένουν αμετάφραστα.",
	  "HATA: çeviri motoru bulunamadı; sözlükte olmayan metinler çevrilmeden kalır.",
	  "FOUT: vertaalmodule niet gevonden; teksten buiten de woordenboeken blijven onvertaald.",
	  "FEL: översättningsmotorn hittades inte; texter utanför ordlistorna förblir oöversatta.",
	  "FEIL: oversettelsesmotoren ble ikke funnet; tekster utenfor ordlistene blir ikke oversatt.",
	  "VIRHE: käännösmoottoria ei löydy; sanastojen ulkopuoliset tekstit jäävät kääntämättä.",
	  "エラー: 翻訳エンジンが見つかりません。辞書にないテキストは翻訳されません。",
	  "오류: 번역 엔진을 찾을 수 없습니다. 사전에 없는 텍스트는 번역되지 않습니다.", "错误：找不到翻译引擎；词典以外的文本将保持原文。" },
	// R_KEYS_OPENED
	{ "cles.json ouvert dans le Bloc-notes : enregistrez-le, la clé sert dès la traduction suivante.",
	  "cles.json opened in Notepad: save it, the key is used from the next translation.",
	  "cles.json im Editor geöffnet: Speichern Sie die Datei, der Schlüssel gilt ab der nächsten Übersetzung.",
	  "cles.json abierto en el Bloc de notas: guárdelo, la clave se usará desde la siguiente traducción.",
	  "cles.json aberto no Bloco de notas: guarde-o, a chave é usada a partir da tradução seguinte.",
	  "cles.json aperto nel Blocco note: salvalo, la chiave vale dalla traduzione successiva.",
	  "A cles.json megnyílt a Jegyzettömbben: mentse, a kulcs a következő fordítástól érvényes.",
	  "cles.json otevřen v Poznámkovém bloku: uložte jej, klíč platí od dalšího překladu.",
	  "cles.json otwarty w Notatniku: zapisz go, klucz zadziała od następnego tłumaczenia.",
	  "cles.json открыт в Блокноте: сохраните его, ключ начнёт работать со следующего перевода.",
	  "cles.json відкрито в Блокноті: збережіть його, ключ діятиме з наступного перекладу.",
	  "Το cles.json άνοιξε στο Σημειωματάριο: αποθηκεύστε το, το κλειδί ισχύει από την επόμενη μετάφραση.",
	  "cles.json Not Defteri'nde açıldı: kaydedin, anahtar bir sonraki çeviriden itibaren kullanılır.",
	  "cles.json geopend in Kladblok: sla het op, de sleutel geldt vanaf de volgende vertaling.",
	  "cles.json öppnad i Anteckningar: spara filen, nyckeln används från nästa översättning.",
	  "cles.json åpnet i Notisblokk: lagre filen, nøkkelen brukes fra neste oversettelse.",
	  "cles.json avattu Muistiossa: tallenna se, avain on käytössä seuraavasta käännöksestä alkaen.",
	  "cles.json をメモ帳で開きました。保存すると、次の翻訳からキーが使われます。",
	  "cles.json을 메모장에서 열었습니다. 저장하면 다음 번역부터 키가 사용됩니다.", "已在记事本中打开 cles.json：保存后，从下一次翻译起使用该密钥。" },
	// R_CORRECTIONS_OPENED
	{ "corrections.json ouvert dans le Bloc-notes : enregistrez-le, les fenêtres ouvertes suivent aussitôt.",
	  "corrections.json opened in Notepad: save it, open windows follow immediately.",
	  "corrections.json im Editor geöffnet: Speichern Sie die Datei, geöffnete Fenster folgen sofort.",
	  "corrections.json abierto en el Bloc de notas: guárdelo, las ventanas abiertas se actualizan al instante.",
	  "corrections.json aberto no Bloco de notas: guarde-o, as janelas abertas acompanham de imediato.",
	  "corrections.json aperto nel Blocco note: salvalo, le finestre aperte si aggiornano subito.",
	  "A corrections.json megnyílt a Jegyzettömbben: mentse, a nyitott ablakok azonnal követik.",
	  "corrections.json otevřen v Poznámkovém bloku: uložte jej, otevřená okna se hned přizpůsobí.",
	  "corrections.json otwarty w Notatniku: zapisz go, otwarte okna dostosują się od razu.",
	  "corrections.json открыт в Блокноте: сохраните его, открытые окна обновятся сразу.",
	  "corrections.json відкрито в Блокноті: збережіть його, відкриті вікна оновляться одразу.",
	  "Το corrections.json άνοιξε στο Σημειωματάριο: αποθηκεύστε το, τα ανοιχτά παράθυρα ενημερώνονται αμέσως.",
	  "corrections.json Not Defteri'nde açıldı: kaydedin, açık pencereler hemen güncellenir.",
	  "corrections.json geopend in Kladblok: sla het op, open vensters volgen direct.",
	  "corrections.json öppnad i Anteckningar: spara filen, öppna fönster följer direkt.",
	  "corrections.json åpnet i Notisblokk: lagre filen, åpne vinduer følger straks.",
	  "corrections.json avattu Muistiossa: tallenna se, avoimet ikkunat päivittyvät heti.",
	  "corrections.json をメモ帳で開きました。保存すると、開いているウィンドウにすぐ反映されます。",
	  "corrections.json을 메모장에서 열었습니다. 저장하면 열린 창에 즉시 반영됩니다.", "已在记事本中打开 corrections.json：保存后，已打开的窗口立即更新。" },
	// R_INTERFACE
	{ "Langue de Tapir Translator : {l}. Messages : tout de suite ; menus : au prochain démarrage d'ArchiCAD.",
	  "Tapir Translator language: {l}. Messages: right away; menus: at the next ArchiCAD start.",
	  "Sprache von Tapir Translator: {l}. Meldungen: sofort; Menüs: beim nächsten Start von ArchiCAD.",
	  "Idioma de Tapir Translator: {l}. Mensajes: al instante; menús: al próximo inicio de ArchiCAD.",
	  "Idioma do Tapir Translator: {l}. Mensagens: de imediato; menus: no próximo arranque do ArchiCAD.",
	  "Lingua di Tapir Translator: {l}. Messaggi: subito; menu: al prossimo avvio di ArchiCAD.",
	  "Tapir Translator nyelve: {l}. Üzenetek: azonnal; menük: az ArchiCAD következő indításakor.",
	  "Jazyk Tapir Translator: {l}. Zprávy: hned; nabídky: při příštím spuštění ArchiCADu.",
	  "Język Tapir Translator: {l}. Komunikaty: od razu; menu: przy następnym uruchomieniu ArchiCADa.",
	  "Язык Tapir Translator: {l}. Сообщения: сразу; меню: при следующем запуске ArchiCAD.",
	  "Мова Tapir Translator: {l}. Повідомлення: одразу; меню: під час наступного запуску ArchiCAD.",
	  "Γλώσσα του Tapir Translator: {l}. Μηνύματα: αμέσως· μενού: στην επόμενη εκκίνηση του ArchiCAD.",
	  "Tapir Translator dili: {l}. Mesajlar: hemen; menüler: ArchiCAD bir sonraki açılışta.",
	  "Taal van Tapir Translator: {l}. Meldingen: direct; menu's: bij de volgende start van ArchiCAD.",
	  "Språk för Tapir Translator: {l}. Meddelanden: direkt; menyer: vid nästa start av ArchiCAD.",
	  "Språk for Tapir Translator: {l}. Meldinger: straks; menyer: ved neste oppstart av ArchiCAD.",
	  "Tapir Translatorin kieli: {l}. Viestit: heti; valikot: ArchiCADin seuraavassa käynnistyksessä.",
	  "Tapir Translator の言語: {l}。メッセージはすぐに、メニューは次回の ArchiCAD 起動時に切り替わります。",
	  "Tapir Translator 언어: {l}. 메시지는 즉시, 메뉴는 다음 ArchiCAD 시작 시 적용됩니다.",
	  "Tapir Translator 语言：{l}。消息立即生效；菜单在下次启动 ArchiCAD 时生效。" },
	// R_KEY_OK
	{ "clé renseignée", "key set", "Schlüssel eingetragen", "clave introducida", "chave indicada", "chiave inserita", "kulcs megadva",
	  "klíč zadán", "klucz podany", "ключ указан", "ключ вказано", "το κλειδί έχει οριστεί", "anahtar girildi", "sleutel ingevuld",
	  "nyckel angiven", "nøkkel angitt", "avain annettu", "キー設定済み", "키 입력됨", "已填写密钥" },
	// R_KEY_TO_ENTER
	{ "CLÉ À SAISIR", "KEY TO ENTER", "SCHLÜSSEL FEHLT", "FALTA LA CLAVE", "FALTA A CHAVE", "CHIAVE MANCANTE", "KULCS HIÁNYZIK",
	  "CHYBÍ KLÍČ", "BRAK KLUCZA", "НУЖЕН КЛЮЧ", "ПОТРІБЕН КЛЮЧ", "ΛΕΙΠΕΙ ΤΟ ΚΛΕΙΔΙ", "ANAHTAR GİRİLMELİ", "SLEUTEL ONTBREEKT",
	  "NYCKEL SAKNAS", "NØKKEL MANGLER", "AVAIN PUUTTUU", "キー未設定", "키 입력 필요", "需要填写密钥" },
	// R_EMAIL_OK
	{ "sans clé, e-mail renseigné", "no key, e-mail set", "ohne Schlüssel, E-Mail eingetragen", "sin clave, correo introducido",
	  "sem chave, e-mail indicado", "senza chiave, e-mail inserita", "kulcs nélkül, e-mail megadva", "bez klíče, e-mail zadán",
	  "bez klucza, e-mail podany", "без ключа, почта указана", "без ключа, пошту вказано", "χωρίς κλειδί, email ορίστηκε",
	  "anahtarsız, e-posta girildi", "zonder sleutel, e-mail ingevuld", "utan nyckel, e-post angiven", "uten nøkkel, e-post angitt",
	  "ilman avainta, sähköposti annettu", "キー不要、メール設定済み", "키 없음, 이메일 입력됨", "无需密钥，已填写邮箱" },
	// R_NO_KEY
	{ "sans clé (quota anonyme)", "no key (anonymous quota)", "ohne Schlüssel (anonymes Kontingent)", "sin clave (cuota anónima)",
	  "sem chave (quota anónima)", "senza chiave (quota anonima)", "kulcs nélkül (névtelen keret)", "bez klíče (anonymní kvóta)",
	  "bez klucza (limit anonimowy)", "без ключа (анонимная квота)", "без ключа (анонімна квота)", "χωρίς κλειδί (ανώνυμο όριο)",
	  "anahtarsız (anonim kota)", "zonder sleutel (anoniem quotum)", "utan nyckel (anonym kvot)", "uten nøkkel (anonym kvote)",
	  "ilman avainta (anonyymi kiintiö)", "キー不要（匿名の上限）", "키 없음(익명 한도)", "无需密钥（匿名配额）" },
	// R_ADDRESS_OK
	{ "adresse renseignée", "address set", "Adresse eingetragen", "dirección introducida", "endereço indicado", "indirizzo inserito",
	  "cím megadva", "adresa zadána", "adres podany", "адрес указан", "адресу вказано", "η διεύθυνση έχει οριστεί", "adres girildi",
	  "adres ingevuld", "adress angiven", "adresse angitt", "osoite annettu", "アドレス設定済み", "주소 입력됨", "已填写地址" },
	// R_ADDRESS_TO_ENTER
	{ "ADRESSE À SAISIR", "ADDRESS TO ENTER", "ADRESSE FEHLT", "FALTA LA DIRECCIÓN", "FALTA O ENDEREÇO", "INDIRIZZO MANCANTE",
	  "CÍM HIÁNYZIK", "CHYBÍ ADRESA", "BRAK ADRESU", "НУЖЕН АДРЕС", "ПОТРІБНА АДРЕСА", "ΛΕΙΠΕΙ Η ΔΙΕΥΘΥΝΣΗ", "ADRES GİRİLMELİ",
	  "ADRES ONTBREEKT", "ADRESS SAKNAS", "ADRESSE MANGLER", "OSOITE PUUTTUU", "アドレス未設定", "주소 입력 필요", "需要填写地址" },
	// C_TITLE
	{ "Corrections des termes", "Term corrections", "Begriffskorrekturen", "Corrección de términos", "Correção de termos",
	  "Correzione dei termini", "Kifejezések javítása", "Opravy termínů", "Poprawki terminów", "Исправление терминов",
	  "Виправлення термінів", "Διορθώσεις όρων", "Terim düzeltmeleri", "Termcorrecties", "Termkorrigeringar", "Termrettelser",
	  "Termikorjaukset", "用語の修正", "용어 수정", "术语更正" },
	// C_SEARCH
	{ "Rechercher", "Search", "Suchen", "Buscar", "Pesquisar", "Cerca", "Keresés", "Hledat", "Szukaj", "Поиск", "Пошук",
	  "Αναζήτηση", "Ara", "Zoeken", "Sök", "Søk", "Haku", "検索", "검색", "搜索" },
	// C_RULES
	{ "Règles ({n})", "Rules ({n})", "Regeln ({n})", "Reglas ({n})", "Regras ({n})", "Regole ({n})", "Szabályok ({n})",
	  "Pravidla ({n})", "Reguły ({n})", "Правила ({n})", "Правила ({n})", "Κανόνες ({n})", "Kurallar ({n})", "Regels ({n})",
	  "Regler ({n})", "Regler ({n})", "Säännöt ({n})", "ルール ({n})", "규칙 ({n})", "规则 ({n})" },
	// C_COL_SOURCE
	{ "Si l'original contient", "If the original contains", "Wenn das Original enthält", "Si el original contiene",
	  "Se o original contém", "Se l'originale contiene", "Ha az eredeti tartalmazza", "Pokud originál obsahuje",
	  "Jeśli oryginał zawiera", "Если оригинал содержит", "Якщо оригінал містить", "Αν το πρωτότυπο περιέχει",
	  "Orijinal şunu içeriyorsa", "Als het origineel bevat", "Om originalet innehåller", "Hvis originalen inneholder",
	  "Jos alkuperäinen sisältää", "原文に含まれる語", "원문에 포함된 단어", "原文包含" },
	// C_COL_WRONG
	{ "Mauvais", "Wrong", "Falsch", "Incorrecto", "Incorreto", "Errato", "Hibás", "Chybně", "Błędnie", "Неверно", "Неправильно",
	  "Λάθος", "Yanlış", "Fout", "Fel", "Feil", "Väärin", "誤", "잘못된 표현", "错误" },
	// C_COL_CORRECT
	{ "Correct", "Correct", "Richtig", "Correcto", "Correto", "Corretto", "Helyes", "Správně", "Poprawnie", "Верно", "Правильно",
	  "Σωστό", "Doğru", "Juist", "Rätt", "Riktig", "Oikein", "正", "올바른 표현", "正确" },
	// C_COL_COUNT
	{ "Traductions", "Translations", "Übersetzungen", "Traducciones", "Traduções", "Traduzioni", "Fordítások", "Překlady",
	  "Tłumaczenia", "Переводы", "Переклади", "Μεταφράσεις", "Çeviriler", "Vertalingen", "Översättningar", "Oversettelser",
	  "Käännökset", "翻訳数", "번역 수", "译文数" },
	// C_ADD
	{ "Ajouter", "Add", "Hinzufügen", "Añadir", "Adicionar", "Aggiungi", "Hozzáadás", "Přidat", "Dodaj", "Добавить", "Додати",
	  "Προσθήκη", "Ekle", "Toevoegen", "Lägg till", "Legg til", "Lisää", "追加", "추가", "添加" },
	// C_DUPLICATE
	{ "Dupliquer", "Duplicate", "Duplizieren", "Duplicar", "Duplicar", "Duplica", "Másolat", "Duplikovat", "Duplikuj",
	  "Дублировать", "Дублювати", "Αντιγραφή", "Çoğalt", "Dupliceren", "Duplicera", "Dupliser", "Monista", "複製", "복제", "复制" },
	// C_DELETE
	{ "Supprimer", "Delete", "Löschen", "Eliminar", "Eliminar", "Elimina", "Törlés", "Odstranit", "Usuń", "Удалить", "Видалити",
	  "Διαγραφή", "Sil", "Verwijderen", "Ta bort", "Slett", "Poista", "削除", "삭제", "删除" },
	// C_SELECTED
	{ "Règle sélectionnée", "Selected rule", "Ausgewählte Regel", "Regla seleccionada", "Regra selecionada", "Regola selezionata",
	  "Kijelölt szabály", "Vybrané pravidlo", "Wybrana reguła", "Выбранное правило", "Вибране правило", "Επιλεγμένος κανόνας",
	  "Seçili kural", "Geselecteerde regel", "Vald regel", "Valgt regel", "Valittu sääntö", "選択中のルール", "선택한 규칙",
	  "选中的规则" },
	// C_HINT
	{ "Mots séparés par des virgules ; vide = tous les textes. La majuscule initiale est gardée ; en fr, es et pt, le pluriel en -s suit tout seul.",
	  "Words separated by commas; empty = every text. The initial capital is kept; in fr, es and pt the -s plural follows by itself.",
	  "Wörter durch Kommas getrennt; leer = alle Texte. Großschreibung am Anfang bleibt erhalten; in fr, es und pt folgt der Plural auf -s automatisch.",
	  "Palabras separadas por comas; vacío = todos los textos. Se conserva la mayúscula inicial; en fr, es y pt el plural en -s se aplica solo.",
	  "Palavras separadas por vírgulas; vazio = todos os textos. A maiúscula inicial é mantida; em fr, es e pt o plural em -s segue sozinho.",
	  "Parole separate da virgole; vuoto = tutti i testi. La maiuscola iniziale viene mantenuta; in fr, es e pt il plurale in -s segue da solo.",
	  "Vesszővel elválasztott szavak; üres = minden szöveg. A kezdő nagybetű megmarad; fr, es és pt nyelven az -s többes szám magától követi.",
	  "Slova oddělená čárkami; prázdné = všechny texty. Velké počáteční písmeno se zachová; ve fr, es a pt se množné číslo na -s doplní samo.",
	  "Słowa oddzielone przecinkami; puste = wszystkie teksty. Wielka litera na początku jest zachowana; w fr, es i pt liczba mnoga na -s dodaje się sama.",
	  "Слова через запятую; пусто = все тексты. Заглавная буква в начале сохраняется; в fr, es и pt множественное число на -s добавляется само.",
	  "Слова через кому; порожньо = усі тексти. Велика літера на початку зберігається; у fr, es і pt множина на -s додається сама.",
	  "Λέξεις χωρισμένες με κόμματα· κενό = όλα τα κείμενα. Το αρχικό κεφαλαίο διατηρείται· στα fr, es και pt ο πληθυντικός σε -s ακολουθεί αυτόματα.",
	  "Virgülle ayrılmış sözcükler; boş = tüm metinler. Baştaki büyük harf korunur; fr, es ve pt dillerinde -s çoğulu kendiliğinden uygulanır.",
	  "Woorden gescheiden door komma's; leeg = alle teksten. De beginhoofdletter blijft behouden; in fr, es en pt volgt het meervoud op -s vanzelf.",
	  "Ord åtskilda med kommatecken; tomt = alla texter. Inledande versal behålls; på fr, es och pt följer pluralis på -s automatiskt.",
	  "Ord adskilt med komma; tomt = alle tekster. Stor forbokstav beholdes; på fr, es og pt følger flertall på -s automatisk.",
	  "Sanat pilkuin erotettuina; tyhjä = kaikki tekstit. Alkuiso kirjain säilyy; kielissä fr, es ja pt -s-monikko seuraa automaattisesti.",
	  "語はカンマ区切り。空欄 = すべてのテキスト。先頭の大文字は保持され、fr・es・pt では -s の複数形も自動で対応します。",
	  "단어는 쉼표로 구분, 비우면 모든 텍스트. 첫 글자 대문자는 유지되며 fr, es, pt에서는 -s 복수형도 자동으로 처리됩니다.",
	  "词语用逗号分隔；留空 = 所有文本。保留首字母大写；在 fr、es、pt 中 -s 复数形式会自动处理。" },
	// C_CACHE
	{ "Traductions en ligne déjà reçues", "Online translations already received", "Bereits erhaltene Online-Übersetzungen",
	  "Traducciones en línea ya recibidas", "Traduções online já recebidas", "Traduzioni online già ricevute",
	  "Már megkapott online fordítások", "Již přijaté online překlady", "Otrzymane już tłumaczenia online",
	  "Уже полученные онлайн-переводы", "Уже отримані онлайн-переклади", "Διαδικτυακές μεταφράσεις που έχουν ληφθεί",
	  "Alınmış çevrimiçi çeviriler", "Al ontvangen onlinevertalingen", "Redan mottagna onlineöversättningar",
	  "Allerede mottatte nettoversettelser", "Jo vastaanotetut verkkokäännökset", "受信済みのオンライン翻訳", "이미 받은 온라인 번역",
	  "已收到的在线翻译" },
	// C_CACHE_HINT
	{ "Sélectionnez une traduction, surlignez le mot mal traduit, puis « Corriger ce mot ».",
	  "Select a translation, highlight the wrongly translated word, then click \xE2\x80\x9C" "Correct this word\xE2\x80\x9D.",
	  "Übersetzung auswählen, falsch übersetztes Wort markieren, dann \xE2\x80\x9E" "Dieses Wort korrigieren\xE2\x80\x9C.",
	  "Seleccione una traducción, resalte la palabra mal traducida y pulse «Corregir esta palabra».",
	  "Selecione uma tradução, realce a palavra mal traduzida e clique em «Corrigir esta palavra».",
	  "Selezionare una traduzione, evidenziare la parola tradotta male, poi «Correggi questa parola».",
	  "Válasszon fordítást, jelölje ki a rosszul fordított szót, majd: \xE2\x80\x9E" "Szó javítása\xE2\x80\x9D.",
	  "Vyberte překlad, označte špatně přeložené slovo a klikněte na \xE2\x80\x9E" "Opravit toto slovo\xE2\x80\x9C.",
	  "Wybierz tłumaczenie, zaznacz źle przetłumaczone słowo, a potem kliknij \xE2\x80\x9E" "Popraw to słowo\xE2\x80\x9D.",
	  "Выберите перевод, выделите неверно переведённое слово и нажмите «Исправить это слово».",
	  "Виберіть переклад, виділіть неправильно перекладене слово й натисніть «Виправити це слово».",
	  "Επιλέξτε μια μετάφραση, επισημάνετε τη λάθος λέξη και πατήστε «Διόρθωση λέξης».",
	  "Bir çeviri seçin, yanlış çevrilen sözcüğü işaretleyin, sonra \xE2\x80\x9C" "Bu sözcüğü düzelt\xE2\x80\x9D.",
	  "Kies een vertaling, markeer het verkeerd vertaalde woord en klik op \xE2\x80\x98" "Dit woord corrigeren\xE2\x80\x99.",
	  "Välj en översättning, markera det felöversatta ordet och klicka på \xE2\x80\x9D" "Rätta ordet\xE2\x80\x9D.",
	  "Velg en oversettelse, merk det feiloversatte ordet og klikk «Rett dette ordet».",
	  "Valitse käännös, maalaa väärin käännetty sana ja valitse \xE2\x80\x9D" "Korjaa tämä sana\xE2\x80\x9D.",
	  "翻訳を選び、誤訳の語を選択して「この語を修正」を押します。",
	  "번역을 고르고 잘못 번역된 단어를 선택한 뒤 '이 단어 수정'을 누르세요.",
	  "选择一条译文，选中译错的词，然后点击\xE2\x80\x9C" "更正此词\xE2\x80\x9D。" },
	// C_CORRECT_WORD
	{ "Corriger ce mot", "Correct this word", "Dieses Wort korrigieren", "Corregir esta palabra", "Corrigir esta palavra",
	  "Correggi questa parola", "Szó javítása", "Opravit toto slovo", "Popraw to słowo", "Исправить это слово",
	  "Виправити це слово", "Διόρθωση λέξης", "Bu sözcüğü düzelt", "Dit woord corrigeren", "Rätta ordet", "Rett dette ordet",
	  "Korjaa tämä sana", "この語を修正", "이 단어 수정", "更正此词" },
	// C_COL_ORIGINAL
	{ "Original", "Original", "Original", "Original", "Original", "Originale", "Eredeti", "Originál", "Oryginał", "Оригинал",
	  "Оригінал", "Πρωτότυπο", "Orijinal", "Origineel", "Original", "Original", "Alkuperäinen", "原文", "원문", "原文" },
	// C_COL_ONLINE
	{ "Traduction en ligne", "Online translation", "Online-Übersetzung", "Traducción en línea", "Tradução online",
	  "Traduzione online", "Online fordítás", "Online překlad", "Tłumaczenie online", "Онлайн-перевод", "Онлайн-переклад",
	  "Διαδικτυακή μετάφραση", "Çevrimiçi çeviri", "Onlinevertaling", "Onlineöversättning", "Nettoversettelse", "Verkkokäännös",
	  "オンライン翻訳", "온라인 번역", "在线翻译" },
	// C_COL_AFTER
	{ "Après corrections", "After corrections", "Nach Korrektur", "Tras las correcciones", "Após as correções",
	  "Dopo le correzioni", "Javítás után", "Po opravách", "Po poprawkach", "После исправлений", "Після виправлень",
	  "Μετά τις διορθώσεις", "Düzeltmeden sonra", "Na correcties", "Efter rättelser", "Etter rettelser", "Korjausten jälkeen",
	  "修正後", "수정 후", "更正后" },
	// C_BEFORE
	{ "Avant", "Before", "Vorher", "Antes", "Antes", "Prima", "Előtte", "Před", "Przed", "До", "До", "Πριν", "Önce", "Voor",
	  "Före", "Før", "Ennen", "修正前", "수정 전", "更正前" },
	// C_AFTER
	{ "Après", "After", "Nachher", "Después", "Depois", "Dopo", "Utána", "Po", "Po", "После", "Після", "Μετά", "Sonra", "Na",
	  "Efter", "Etter", "Jälkeen", "修正後", "수정 후", "更正后" },
	// C_LIVE
	{ "Appliqué en direct aux fenêtres ouvertes et au cache", "Applied live to open windows and to the cache",
	  "Wird sofort auf offene Fenster und den Cache angewendet", "Se aplica en directo a las ventanas abiertas y a la caché",
	  "Aplicado em direto às janelas abertas e à cache", "Applicato subito alle finestre aperte e alla cache",
	  "Azonnal érvényes a nyitott ablakokra és a gyorsítótárra", "Ihned platí pro otevřená okna i mezipaměť",
	  "Stosowane od razu w otwartych oknach i pamięci podręcznej", "Сразу применяется к открытым окнам и кэшу",
	  "Одразу застосовується до відкритих вікон і кешу", "Εφαρμόζεται αμέσως στα ανοιχτά παράθυρα και στην προσωρινή μνήμη",
	  "Açık pencerelere ve önbelleğe hemen uygulanır", "Direct toegepast op open vensters en de cache",
	  "Tillämpas direkt på öppna fönster och cachen", "Brukes straks på åpne vinduer og hurtigbufferen",
	  "Käytetään heti avoimiin ikkunoihin ja välimuistiin", "開いているウィンドウとキャッシュに即時適用", "열린 창과 캐시에 즉시 적용",
	  "立即应用于打开的窗口和缓存" },
	// C_CANCEL
	{ "Annuler", "Cancel", "Abbrechen", "Cancelar", "Cancelar", "Annulla", "Mégse", "Zrušit", "Anuluj", "Отмена", "Скасувати",
	  "Άκυρο", "İptal", "Annuleren", "Avbryt", "Avbryt", "Peruuta", "キャンセル", "취소", "取消" },
	// C_UNSAVED
	{ "Enregistrer les modifications avant de fermer ?", "Save the changes before closing?",
	  "Änderungen vor dem Schließen speichern?", "¿Guardar los cambios antes de cerrar?", "Guardar as alterações antes de fechar?",
	  "Salvare le modifiche prima di chiudere?", "Menti a módosításokat bezárás előtt?", "Uložit změny před zavřením?",
	  "Zapisać zmiany przed zamknięciem?", "Сохранить изменения перед закрытием?", "Зберегти зміни перед закриттям?",
	  "Αποθήκευση αλλαγών πριν το κλείσιμο;", "Kapatmadan önce değişiklikler kaydedilsin mi?", "Wijzigingen opslaan voor het sluiten?",
	  "Spara ändringarna innan du stänger?", "Lagre endringene før lukking?", "Tallennetaanko muutokset ennen sulkemista?",
	  "閉じる前に変更を保存しますか?", "닫기 전에 변경 내용을 저장할까요?", "关闭前保存更改？" },
	// C_UNREADABLE
	{ "corrections.json est illisible : corrigez-le dans le Bloc-notes ou supprimez-le.",
	  "corrections.json is unreadable: fix it in Notepad or delete it.",
	  "corrections.json ist nicht lesbar: im Editor korrigieren oder löschen.",
	  "corrections.json no se puede leer: corríjalo en el Bloc de notas o elimínelo.",
	  "corrections.json está ilegível: corrija-o no Bloco de Notas ou elimine-o.",
	  "corrections.json non è leggibile: correggilo nel Blocco note o eliminalo.",
	  "A corrections.json nem olvasható: javítsa a Jegyzettömbben vagy törölje.",
	  "Soubor corrections.json nelze číst: opravte ho v Poznámkovém bloku nebo ho smažte.",
	  "Pliku corrections.json nie można odczytać: popraw go w Notatniku lub usuń.",
	  "Файл corrections.json не читается: исправьте его в Блокноте или удалите.",
	  "Файл corrections.json не читається: виправте його в Блокноті або видаліть.",
	  "Το corrections.json δεν διαβάζεται: διορθώστε το στο Σημειωματάριο ή διαγράψτε το.",
	  "corrections.json okunamıyor: Not Defteri'nde düzeltin veya silin.",
	  "corrections.json is onleesbaar: corrigeer het in Kladblok of verwijder het.",
	  "corrections.json kan inte läsas: rätta den i Anteckningar eller ta bort den.",
	  "corrections.json kan ikke leses: rett den i Notisblokk eller slett den.",
	  "corrections.json ei ole luettavissa: korjaa se Muistiossa tai poista se.",
	  "corrections.json を読み込めません。メモ帳で修正するか削除してください。",
	  "corrections.json을 읽을 수 없습니다. 메모장에서 고치거나 삭제하세요.",
	  "无法读取 corrections.json：请用记事本修正或将其删除。" },
	// C_SAVE_ERROR
	{ "Impossible d'enregistrer corrections.json : ", "Cannot save corrections.json: ",
	  "corrections.json kann nicht gespeichert werden: ", "No se puede guardar corrections.json: ",
	  "Não é possível guardar corrections.json: ", "Impossibile salvare corrections.json: ", "A corrections.json nem menthető: ",
	  "Soubor corrections.json nelze uložit: ", "Nie można zapisać corrections.json: ", "Не удаётся сохранить corrections.json: ",
	  "Не вдається зберегти corrections.json: ", "Δεν είναι δυνατή η αποθήκευση του corrections.json: ",
	  "corrections.json kaydedilemiyor: ", "corrections.json kan niet worden opgeslagen: ", "corrections.json kan inte sparas: ",
	  "corrections.json kan ikke lagres: ", "corrections.json-tiedostoa ei voi tallentaa: ", "corrections.json を保存できません: ",
	  "corrections.json을 저장할 수 없습니다: ", "无法保存 corrections.json：" },
	// C_ALL
	{ "(tous les textes)", "(every text)", "(alle Texte)", "(todos los textos)", "(todos os textos)", "(tutti i testi)",
	  "(minden szöveg)", "(všechny texty)", "(wszystkie teksty)", "(все тексты)", "(усі тексти)", "(όλα τα κείμενα)",
	  "(tüm metinler)", "(alle teksten)", "(alla texter)", "(alle tekster)", "(kaikki tekstit)", "(すべてのテキスト)",
	  "(모든 텍스트)", "（所有文本）" },
};

// Language code of each interface index (same order as the columns above).
const char* const kCodes[InterfaceLanguageCount] = { "fr", "en", "de", "es", "pt", "it", "hu", "cs", "pl", "ru",
													 "uk", "el", "tr", "nl", "sv", "no", "fi", "ja", "ko", "zh" };

// Name of each interface language, written in that language.
const char* const kNames[InterfaceLanguageCount] = { "Français", "English", "Deutsch", "Español", "Português", "Italiano", "Magyar",
													"Čeština", "Polski", "Русский", "Українська", "Ελληνικά", "Türkçe", "Nederlands",
													"Svenska", "Norsk", "Suomi", "日本語", "한국어", "中文" };

}	// namespace


// Counts the null or empty cells of the table (the test bench requires 0).
int MissingTexts ()
{
	int n = 0;
	for (int t = 0; t < TextCount; ++t) {
		for (int l = 0; l < InterfaceLanguageCount; ++l) {
			if (kTexts[t][l] == nullptr || kTexts[t][l][0] == '\0')
				++n;
		}
	}
	return n;
}


// Language code of interface index i ("en" if out of range).
const char* InterfaceCode (int i)
{
	return i >= 0 && i < InterfaceLanguageCount ? kCodes[i] : "en";
}


// Native name of interface index i ("English" if out of range).
const char* InterfaceName (int i)
{
	return i >= 0 && i < InterfaceLanguageCount ? kNames[i] : "English";
}


// Looks up one text (see TT_Texts.hpp). Never returns nullptr.
const char* Txt (Text t, int language)
{
	// Step 1: invalid text identifier -> empty string.
	if (t < 0 || t >= TextCount)
		return "";
	// Step 2: invalid language index -> English (index 1).
	if (language < 0 || language >= InterfaceLanguageCount)
		language = 1;
	// Step 3: the cell, or the English cell if it is empty.
	const char* r = kTexts[t][language];
	if (r == nullptr || *r == '\0')
		r = kTexts[t][1];		// missing cell: English
	return r != nullptr ? r : "";
}


// Looks up one text and fills its placeholders (see TT_Texts.hpp). Every occurrence of each
// {key} is replaced; the search resumes after the inserted value, so a value that itself
// contains the same {key} is not expanded again (no infinite loop).
std::string TxtF (Text t, int language, std::initializer_list<std::pair<const char*, std::string>> values)
{
	std::string r = Txt (t, language);
	for (const auto& v : values) {
		const std::string key = std::string ("{") + v.first + "}";
		size_t p = 0;
		while ((p = r.find (key, p)) != std::string::npos) {
			r.replace (p, key.size (), v.second);
			p += v.second.size ();
		}
	}
	return r;
}


// Interface index of a language code; unknown (or empty) code -> English (1).
int InterfaceIndex (const std::string& code)
{
	for (int i = 0; i < InterfaceLanguageCount; ++i) {
		if (code == kCodes[i])
			return i;
	}
	return 1;
}


// Maps the language code of an ArchiCAD localised version to a language code of the tool.
// Several versions share a language (GER / AUT / CHE -> de, POR / BRA -> pt, CHI / TAI -> zh,
// INT / USA / UKI / AUS / NZE -> en). Any other version -> "en".
std::string LanguageFromArchicad (const std::string& version)
{
	static const char* const table[][2] = {
		{ "FRA", "fr" }, { "GER", "de" }, { "AUT", "de" }, { "CHE", "de" }, { "SPA", "es" }, { "POR", "pt" }, { "BRA", "pt" },
		{ "ITA", "it" }, { "HUN", "hu" }, { "CZE", "cs" }, { "POL", "pl" }, { "RUS", "ru" }, { "UKR", "uk" }, { "GRE", "el" },
		{ "TUR", "tr" }, { "NED", "nl" }, { "SWE", "sv" }, { "NOR", "no" }, { "FIN", "fi" }, { "JPN", "ja" }, { "KOR", "ko" },
		{ "CHI", "zh" }, { "TAI", "zh" },
		{ "INT", "en" }, { "USA", "en" }, { "UKI", "en" }, { "AUS", "en" }, { "NZE", "en" },
	};
	for (const auto& l : table) {
		if (version == l[0])
			return l[1];
	}
	return "en";
}

}	// namespace TT
