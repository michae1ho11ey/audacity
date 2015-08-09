/**********************************************************************

  Audacity: A Digital Audio Editor

  CommandManager.cpp

  Brian Gunlogson
  Dominic Mazzoni

*******************************************************************//**

\class CommandManager
\brief CommandManager implements a system for organizing all user-callable
  commands.

  It creates and manages a menu bar with a command
  associated with each item, and managing other commands callable
  by keyboard shortcuts.

  Commands are implemented by overriding an abstract functor class.
  See Menus.cpp for an example use.

  Menus or submenus containing lists of items can be added at once,
  with a single function (functor) to be called when any of the
  items is selected, with the index number of the selection as the
  parameter.  This is useful for dynamic menus (effects) and
  submenus containing a list of choices (selection formats).

  Menu items can be enabled or disabled individually, groups of
  "multi-items" can be enabled or disabled all at once, or entire
  sets of commands can be enabled or disabled all at once using
  flags.  The flags should be a bitfield stored in a 32-bit
  integer but can be whatever you want.  You specify both the
  desired values of the flags, and the set of flags relevant to
  a particular command, by using a combination of a flags parameter
  and a mask parameter.  Any flag set to 0 in the mask parameter is
  the same as "don't care".  Any command whose mask is set to zero
  will not be affected by enabling/disabling by flags.

*//****************************************************************//**

\class CommandFunctor
\brief CommandFunctor is a very small class that works with
CommandManager.  It holds the callback for one command.

*//****************************************************************//**

\class MenuBarListEntry
\brief MenuBarListEntry is a structure used by CommandManager.

*//****************************************************************//**

\class SubMenuListEntry
\brief SubMenuListEntry is a structure used by CommandManager.

*//****************************************************************//**

\class CommandListEntry
\brief CommandListEntry is a structure used by CommandManager.

*//****************************************************************//**

\class MenuBarList
\brief List of MenuBarListEntry.

*//****************************************************************//**

\class SubMenuList
\brief List of SubMenuListEntry.

*//****************************************************************//**

\class CommandList
\brief List of CommandListEntry.

*//******************************************************************/

#include "../Audacity.h"

#include <wx/defs.h>
#include <wx/hash.h>
#include <wx/intl.h>
#include <wx/log.h>
#include <wx/msgdlg.h>
#include <wx/tokenzr.h>

#include "../AudacityApp.h"
#include "../Prefs.h"
#include "../Project.h"

#include "CommandManager.h"

#include "Keyboard.h"
#include "../PluginManager.h"
#include "../effects/EffectManager.h"

// On wxGTK, there may be many many many plugins, but the menus don't automatically
// allow for scrolling, so we build sub-menus.  If the menu gets longer than
// MAX_MENU_LEN, we put things in submenus that have MAX_SUBMENU_LEN items in them.
//
#ifdef __WXGTK__
#define MAX_MENU_LEN 20
#define MAX_SUBMENU_LEN 15
#else
#define MAX_MENU_LEN 1000
#define MAX_SUBMENU_LEN 1000
#endif

#define COMMAND _("Command")

#if defined(__WXMAC__)
#include <AppKit/AppKit.h>
#include <wx/osx/private.h>
#elif defined(__WXGTK__)
#include <gtk/gtk.h>
#endif

// Shared by all projects
static class CommandManagerEventMonitor : public wxEventFilter
{
public:
   CommandManagerEventMonitor()
   :  wxEventFilter()
   {
#if defined(__WXMAC__)
      NSEventMask mask = NSKeyDownMask;

      mHandler =
      [
         NSEvent addLocalMonitorForEventsMatchingMask:mask handler:^NSEvent *(NSEvent *event)
         {
            WXWidget widget = (WXWidget) [[event window] firstResponder];
            if (widget)
            {
               wxWidgetCocoaImpl *impl = (wxWidgetCocoaImpl *)
                  wxWidgetImpl::FindFromWXWidget(widget);
               if (impl)
               {
                  mEvent = event;

                  wxKeyEvent wxevent(wxEVT_KEY_DOWN);
                  impl->SetupKeyEvent(wxevent, event);
              
                  wxKeyEvent eventHook(wxEVT_CHAR_HOOK, wxevent);
                  return FilterEvent(eventHook) == Event_Processed ? nil : event;
               }
            }

            return event;
         }
      ];
#else
      wxEvtHandler::AddFilter(this);
#endif
   }

   virtual ~CommandManagerEventMonitor()
   {
#if defined(__WXMAC__)
      [NSEvent removeMonitor:mHandler];
#else
      wxEvtHandler::RemoveFilter(this);
#endif
   }

   int FilterEvent(wxEvent& event)
   {
      wxEventType type = event.GetEventType();
      if (type != wxEVT_CHAR_HOOK)
      {
         return Event_Skip;
      }
 
      AudacityProject *project = GetActiveProject();
      if (!project)
      {
         return Event_Skip;
      }
      
      wxWindow *handler = project->GetKeyboardCaptureHandler();
      if (handler && HandleCapture(handler, (wxKeyEvent &) event))
      {
         return Event_Processed;
      }

      CommandManager *manager = project->GetCommandManager();
      if (manager && manager->FilterKeyEvent(project, (wxKeyEvent &) event))
      {
         return Event_Processed;
      }

      return Event_Skip;
   }

private:

   // Returns true if the event was captured and processed
   bool HandleCapture(wxWindow *target, wxKeyEvent & event)
   {
      if (wxGetTopLevelParent(target) != wxGetTopLevelParent(wxWindow::FindFocus()))
      {
         return false;
      }
      wxEvtHandler *handler = target->GetEventHandler();

      wxKeyEvent temp = (wxKeyEvent &) event;
      temp.SetEventType(wxEVT_KEY_DOWN);

#if defined(__WXGTK__)
      // wxGTK uses the control and alt modifiers to represent ALTGR,
      // so remove it as it might confuse the capture handlers.
      if (temp.GetModifiers() == (wxMOD_CONTROL | wxMOD_ALT))
      {
         temp.SetControlDown(false);
         temp.SetAltDown(false);
      }
#endif

      wxCommandEvent e(EVT_CAPTURE_KEY);
      e.SetEventObject(&temp);
      e.StopPropagation();

      if (!handler->ProcessEvent(e))
      {
         return false;
      }

      temp.WasProcessed();
      temp.StopPropagation();
      wxEventProcessInHandlerOnly onlyDown(temp, handler);
      if (!handler->ProcessEvent(temp))
      {
         return false;
      }

      wxString chars = GetUnicodeString(temp);
      for (size_t i = 0, cnt = chars.Length(); i < cnt; i++)
      {
         temp = (wxKeyEvent &) event;
         temp.SetEventType(wxEVT_CHAR);
         temp.WasProcessed();
         temp.StopPropagation();
         temp.m_uniChar = chars[i];
         wxEventProcessInHandlerOnly onlyChar(temp, handler);
         handler->ProcessEvent(temp);
      }
   
      temp = (wxKeyEvent &) event;
      temp.SetEventType(wxEVT_KEY_UP);
      temp.WasProcessed();
      temp.StopPropagation();
      wxEventProcessInHandlerOnly onlyUp(temp, handler);
      handler->ProcessEvent(temp);

      return true;
   }

   wxString GetUnicodeString(const wxKeyEvent & event)
   {
      wxString chars;

#if defined(__WXMSW__)

      BYTE ks[256];
      GetKeyboardState(ks);
      WCHAR ucode[256];
      int res = ToUnicode(event.GetRawKeyCode(), 0, ks, ucode, 256, 0);
      if (res >= 1)
      {
         chars.Append(ucode, res);
      }

#elif defined(__WXGTK__)

      chars.Append((wxChar) gdk_keyval_to_unicode(event.GetRawKeyCode()));
                                     
#elif defined(__WXMAC__)

      NSString *c = [mEvent charactersIgnoringModifiers];
      if ([c length] == 1)
      {
         unichar codepoint = [c characterAtIndex:0];
         if ((codepoint >= 0xF700 && codepoint <= 0xF8FF) || codepoint == 0x7F)
         {
            return chars;
         }
      }

      c = [mEvent characters];
      chars = [c UTF8String];

      TISInputSourceRef currentKeyboard = TISCopyCurrentKeyboardInputSource();
      CFDataRef uchr = (CFDataRef)TISGetInputSourceProperty(currentKeyboard, kTISPropertyUnicodeKeyLayoutData);
      CFRelease(currentKeyboard);
      if (uchr == NULL)
      {
         return chars;
      }

      const UCKeyboardLayout *keyboardLayout = (const UCKeyboardLayout*)CFDataGetBytePtr(uchr);
      if (keyboardLayout == NULL)
      {
         return chars;
      }
      
      const UniCharCount maxStringLength = 255;
      UniCharCount actualStringLength = 0;
      UniChar unicodeString[maxStringLength];
      UInt32 nsflags = [mEvent modifierFlags];
      UInt16 modifiers = (nsflags & NSAlphaShiftKeyMask ? alphaLock : 0) |
                         (nsflags & NSShiftKeyMask ? shiftKey : 0) |
                         (nsflags & NSControlKeyMask ? controlKey : 0) |
                         (nsflags & NSAlternateKeyMask ? optionKey : 0) |
                         (nsflags & NSCommandKeyMask ? cmdKey : 0);
         
      OSStatus status = UCKeyTranslate(keyboardLayout,
                                       [mEvent keyCode],
                                       kUCKeyActionDown,
                                       (modifiers >> 8) & 0xff,
                                       LMGetKbdType(),
                                       0,
                                       &mDeadKeyState,
                                       maxStringLength,
                                       &actualStringLength,
                                       unicodeString);
     
      if (status != noErr)
      {
         return chars;
      }
      
      chars = [[NSString stringWithCharacters:unicodeString
                                       length:(NSInteger)actualStringLength] UTF8String];

#endif

      return chars;
   }

private:

#if defined(__WXMAC__)   
   id mHandler;
   NSEvent *mEvent;
   UInt32 mDeadKeyState;
#endif

} monitor;

///
///  Standard Constructor
///
CommandManager::CommandManager():
   mCurrentID(17000),
   mCurrentMenuName(COMMAND),
   mCurrentMenu(NULL),
   mDefaultFlags(0),
   mDefaultMask(0)
{
   mbSeparatorAllowed = false;
}

///
///  Class Destructor.  Includes PurgeData, which removes
///  menubars
CommandManager::~CommandManager()
{
   //WARNING: This removes menubars that could still be assigned to windows!
   PurgeData();
}

void CommandManager::PurgeData()
{
   // Delete callback functors BEFORE clearing mCommandList!
   // These are the items created within 'FN()'
   size_t i;
   CommandFunctor * pCallback = NULL;
   for(i=0; i<mCommandList.GetCount(); i++)
   {
      CommandListEntry *tmpEntry = mCommandList[i];
      // JKC: We only want to delete each callbacks once.
      // AddItemList() may have inserted the same callback
      // several times over.
      if( tmpEntry->callback != pCallback )
      {
         pCallback = tmpEntry->callback;
         delete pCallback;
      }
   }

   // mCommandList contains pointers to CommandListEntrys
   // mMenuBarList contains pointers to MenuBarListEntrys.
   // mSubMenuList contains pointers to SubMenuListEntrys
   WX_CLEAR_ARRAY( mCommandList );
   WX_CLEAR_ARRAY( mMenuBarList );
   WX_CLEAR_ARRAY( mSubMenuList );

   mCommandNameHash.clear();
   mCommandKeyHash.clear();
   mCommandIDHash.clear();

   mCurrentMenu = NULL;
   mCurrentMenuName = COMMAND;
   mCurrentID = 0;
}


///
/// Makes a new menubar for placement on the top of a project
/// Names it according to the passed-in string argument.
///
/// If the menubar already exists, simply returns it.
wxMenuBar *CommandManager::AddMenuBar(wxString sMenu)
{
   wxMenuBar *menuBar = GetMenuBar(sMenu);
   if (menuBar)
      return menuBar;

   MenuBarListEntry *tmpEntry = new MenuBarListEntry;

   tmpEntry->menubar = new wxMenuBar();
   tmpEntry->name = sMenu;

   mMenuBarList.Add(tmpEntry);

   return tmpEntry->menubar;
}


///
/// Retrieves the menubar based on the name given in AddMenuBar(name)
///
wxMenuBar * CommandManager::GetMenuBar(wxString sMenu) const
{
   for(unsigned int i = 0; i < mMenuBarList.GetCount(); i++)
   {
      if(!mMenuBarList[i]->name.Cmp(sMenu))
         return mMenuBarList[i]->menubar;
   }

   return NULL;
}


///
/// Retrieve the 'current' menubar; either NULL or the
/// last on in the mMenuBarList.
wxMenuBar * CommandManager::CurrentMenuBar() const
{
   if(mMenuBarList.IsEmpty())
      return NULL;

   return mMenuBarList[mMenuBarList.GetCount()-1]->menubar;
}


///
/// This makes a new menu and adds it to the 'CurrentMenuBar'
///
/// If the menu already exists, all of the items in it are
/// cleared instead.
///
void CommandManager::BeginMenu(wxString tNameIn)
{
   wxString tName = tNameIn;

   wxMenu *tmpMenu = new wxMenu();

   mCurrentMenu = tmpMenu;
   mCurrentMenuName = tName;

   CurrentMenuBar()->Append(mCurrentMenu, tName);
}


///
/// This ends a menu by setting the pointer to it
/// to NULL.  It is still attached to the CurrentMenuBar()
void CommandManager::EndMenu()
{
   mCurrentMenu = NULL;
   mCurrentMenuName = COMMAND;
}


///
/// This starts a new submenu, and names it according to
/// the function's argument.
wxMenu* CommandManager::BeginSubMenu(wxString tNameIn)
{
   wxString tName = tNameIn;

   SubMenuListEntry *tmpEntry = new SubMenuListEntry;

   tmpEntry->menu = new wxMenu();
   tmpEntry->name = tName;

   mSubMenuList.Add(tmpEntry);
   mbSeparatorAllowed = false;

   return(tmpEntry->menu);
}


///
/// This function is called after the final item of a SUBmenu is added.
/// Submenu items are added just like regular menu items; they just happen
/// after BeginSubMenu() is called but before EndSubMenu() is called.
void CommandManager::EndSubMenu()
{
   size_t submenu_count = mSubMenuList.GetCount()-1;

   //Save the submenu's information
   SubMenuListEntry *tmpSubMenu = mSubMenuList[submenu_count];

   //Pop off the new submenu so CurrentMenu returns the parent of the submenu
   mSubMenuList.RemoveAt(submenu_count);

   //Add the submenu to the current menu
   CurrentMenu()->Append(0, tmpSubMenu->name, tmpSubMenu->menu, tmpSubMenu->name);
   mbSeparatorAllowed = true;

   delete tmpSubMenu;
}


///
/// This returns the 'Current' Submenu, which is the one at the
///  end of the mSubMenuList (or NULL, if it doesn't exist).
wxMenu * CommandManager::CurrentSubMenu() const
{
   if(mSubMenuList.IsEmpty())
      return NULL;

   return mSubMenuList[mSubMenuList.GetCount()-1]->menu;
}

///
/// This returns the current menu that we're appending to - note that
/// it could be a submenu if BeginSubMenu was called and we haven't
/// reached EndSubMenu yet.
wxMenu * CommandManager::CurrentMenu() const
{
   if(!mCurrentMenu)
      return NULL;

   wxMenu * tmpCurrentSubMenu = CurrentSubMenu();

   if(!tmpCurrentSubMenu)
   {
      return mCurrentMenu;
   }

   return tmpCurrentSubMenu;
}

///
/// Add a menu item to the current menu.  When the user selects it, the
/// given functor will be called
void CommandManager::InsertItem(wxString name, wxString label_in,
                                CommandFunctor *callback, wxString after,
                                int checkmark)
{
   wxString label = label_in;

   wxMenuBar *bar = GetActiveProject()->GetMenuBar();
   wxArrayString names = ::wxStringTokenize(after, wxT(":"));
   size_t cnt = names.GetCount();

   if (cnt < 2) {
      return;
   }

   int pos = bar->FindMenu(names[0]);
   if (pos == wxNOT_FOUND) {
      return;
   }

   wxMenu *menu = bar->GetMenu(pos);
   wxMenuItem *item = NULL;
   pos = 0;

   for (size_t ndx = 1; ndx < cnt; ndx++) {
      wxMenuItemList list = menu->GetMenuItems();
      size_t lcnt = list.GetCount();
      wxString label = wxMenuItem::GetLabelText(names[ndx]);

      for (size_t lndx = 0; lndx < lcnt; lndx++) {
         item = list.Item(lndx)->GetData();
         if (item->GetItemLabelText() == label) {
            break;
         }
         pos++;
         item = NULL;
      }

      if (item == NULL) {
         return;
      }

      if (item->IsSubMenu()) {
         menu = item->GetSubMenu();
         item = NULL;
         continue;
      }

      if (ndx + 1 != cnt) {
         return;
      }
   }

   CommandListEntry *entry = NewIdentifier(name, label, menu, callback, false, 0, 0);
   int ID = entry->id;
   label = GetLabel(entry);

   if (checkmark >= 0) {
      menu->InsertCheckItem(pos, ID, label);
      menu->Check(ID, checkmark != 0);
   }
   else {
      menu->Insert(pos, ID, label);
   }

   mbSeparatorAllowed = true;
}



void CommandManager::AddCheck(const wxChar *name,
                              const wxChar *label,
                              CommandFunctor *callback,
                              int checkmark)
{
   AddItem(name, label, callback, wxT(""), (unsigned int)NoFlagsSpecifed, (unsigned int)NoFlagsSpecifed, checkmark);
}

void CommandManager::AddCheck(const wxChar *name,
                              const wxChar *label,
                              CommandFunctor *callback,
                              int checkmark,
                              unsigned int flags,
                              unsigned int mask)
{
   AddItem(name, label, callback, wxT(""), flags, mask, checkmark);
}

void CommandManager::AddItem(const wxChar *name,
                             const wxChar *label,
                             CommandFunctor *callback,
                             unsigned int flags,
                             unsigned int mask)
{
   AddItem(name, label, callback, wxT(""), flags, mask);
}

void CommandManager::AddItem(const wxChar *name,
                             const wxChar *label_in,
                             CommandFunctor *callback,
                             const wxChar *accel,
                             unsigned int flags,
                             unsigned int mask,
                             int checkmark)
{
   CommandListEntry *entry = NewIdentifier(name, label_in, accel, CurrentMenu(), callback, false, 0, 0);
   int ID = entry->id;
   wxString label = GetLabel(entry);

   if (flags != NoFlagsSpecifed || mask != NoFlagsSpecifed) {
      SetCommandFlags(name, flags, mask);
   }


   if (checkmark >= 0) {
      CurrentMenu()->AppendCheckItem(ID, label);
      CurrentMenu()->Check(ID, checkmark != 0);
   }
   else {
      CurrentMenu()->Append(ID, label);
   }

   mbSeparatorAllowed = true;
}

///
/// Add a list of menu items to the current menu.  When the user selects any
/// one of these, the given functor will be called
/// with its position in the list as the index number.
/// When you call Enable on this command name, it will enable or disable
/// all of the items at once.
void CommandManager::AddItemList(wxString name, wxArrayString labels,
                                 CommandFunctor *callback)
{
   for (size_t i = 0, cnt = labels.GetCount(); i < cnt; i++) {
      CommandListEntry *entry = NewIdentifier(name,
                                              labels[i],
                                              CurrentMenu(),
                                              callback,
                                              true,
                                              i,
                                              cnt);
      CurrentMenu()->Append(entry->id, GetLabel(entry));
      mbSeparatorAllowed = true;
   }
}

///
/// Add a command that doesn't appear in a menu.  When the key is pressed, the
/// given function pointer will be called (via the CommandManagerListener)
void CommandManager::AddCommand(const wxChar *name,
                                const wxChar *label,
                                CommandFunctor *callback,
                                unsigned int flags,
                                unsigned int mask)
{
   AddCommand(name, label, callback, wxT(""), flags, mask);
}

void CommandManager::AddCommand(const wxChar *name,
                                const wxChar *label_in,
                                CommandFunctor *callback,
                                const wxChar *accel,
                                unsigned int flags,
                                unsigned int mask)
{
   NewIdentifier(name, label_in, accel, NULL, callback, false, 0, 0);

   if (flags != NoFlagsSpecifed || mask != NoFlagsSpecifed) {
      SetCommandFlags(name, flags, mask);
   }
}

void CommandManager::AddMetaCommand(const wxChar *name,
                                    const wxChar *label_in,
                                    CommandFunctor *callback,
                                    const wxChar *accel)
{
   CommandListEntry *entry = NewIdentifier(name, label_in, accel, NULL, callback, false, 0, 0);

   entry->enabled = false;
   entry->isMeta = true;
   entry->flags = 0;
   entry->mask = 0;
}

void CommandManager::AddSeparator()
{
   if( mbSeparatorAllowed )
      CurrentMenu()->AppendSeparator();
   mbSeparatorAllowed = false; // boolean to prevent too many separators.
}

int CommandManager::NextIdentifier(int ID)
{
   ID++;

   //Skip the reserved identifiers used by wxWidgets
   if((ID >= wxID_LOWEST) && (ID <= wxID_HIGHEST))
      ID = wxID_HIGHEST+1;

   return ID;
}

///Given all of the information for a command, comes up with a new unique
///ID, adds it to a list, and returns the ID.
///WARNING: Does this conflict with the identifiers set for controls/windows?
///If it does, a workaround may be to keep controls below wxID_LOWEST
///and keep menus above wxID_HIGHEST
CommandListEntry *CommandManager::NewIdentifier(const wxString & name,
                                                const wxString & label,
                                                wxMenu *menu,
                                                CommandFunctor *callback,
                                                bool multi,
                                                int index,
                                                int count)
{
   return NewIdentifier(name,
                        label.BeforeFirst(wxT('\t')),
                        label.AfterFirst(wxT('\t')),
                        menu,
                        callback,
                        multi,
                        index,
                        count);
}

CommandListEntry *CommandManager::NewIdentifier(const wxString & name,
                                                const wxString & label,
                                                const wxString & accel,
                                                wxMenu *menu,
                                                CommandFunctor *callback,
                                                bool multi,
                                                int index,
                                                int count)
{
   CommandListEntry *entry = new CommandListEntry;

   wxString labelPrefix;
   if (!mSubMenuList.IsEmpty()) {
      labelPrefix = mSubMenuList[mSubMenuList.GetCount() - 1]->name;
   }

   // wxMac 2.5 and higher will do special things with the
   // Preferences, Exit (Quit), and About menu items,
   // if we give them the right IDs.
   // Otherwise we just pick increasing ID numbers for each new
   // command.  Note that the name string we are comparing
   // ("About", "Preferences") is the internal command name
   // (untranslated), not the label that actually appears in the
   // menu (which might be translated).

   mCurrentID = NextIdentifier(mCurrentID);
   entry->id = mCurrentID;

#if defined(__WXMAC__)
   if (name == wxT("Preferences"))
      entry->id = wxID_PREFERENCES;
   else if (name == wxT("Exit"))
      entry->id = wxID_EXIT;
   else if (name == wxT("About"))
      entry->id = wxID_ABOUT;
#endif

   entry->defaultKey = entry->key;
   entry->name = name;
   entry->label = label;
   entry->key = KeyStringNormalize(accel.BeforeFirst(wxT('\t')));
   entry->labelPrefix = labelPrefix;
   entry->labelTop = wxMenuItem::GetLabelText(mCurrentMenuName);
   entry->menu = menu;
   entry->callback = callback;
   entry->multi = multi;
   entry->index = index;
   entry->count = count;
   entry->flags = mDefaultFlags;
   entry->mask = mDefaultMask;
   entry->enabled = true;
   entry->wantevent = (accel.Find(wxT("\twantevent")) != wxNOT_FOUND);
   entry->ignoredown = (accel.Find(wxT("\tignoredown")) != wxNOT_FOUND);
   entry->isMeta = false;

   // For key bindings for commands with a list, such as effects,
   // the name in prefs is the category name plus the effect name.
   if (multi) {
      entry->name = wxString::Format( wxT("%s:%s"), name.c_str(), label.c_str() );
   }

   // Key from preferences overridse the default key given
   gPrefs->SetPath(wxT("/NewKeys"));
   if (gPrefs->HasEntry(name)) {
      entry->key = KeyStringNormalize(gPrefs->Read(name, entry->key));
   }
   gPrefs->SetPath(wxT("/"));

   mCommandList.Add(entry);
   mCommandIDHash[entry->id] = entry;

#if defined(__WXDEBUG__)
   CommandListEntry *prev = mCommandNameHash[entry->name];
   if (prev) {
      // Under Linux it looks as if we may ask for a newID for the same command
      // more than once.  So it's only an error if two different commands
      // have the exact same name.
      if( prev->label != entry->label )
      {
         wxLogDebug(wxT("Command '%s' defined by '%s' and '%s'"),
                    entry->name.c_str(),
                    prev->label.BeforeFirst(wxT('\t')).c_str(),
                    entry->label.BeforeFirst(wxT('\t')).c_str());
         wxFAIL_MSG(wxString::Format(wxT("Command '%s' defined by '%s' and '%s'"),
                    entry->name.c_str(),
                    prev->label.BeforeFirst(wxT('\t')).c_str(),
                    entry->label.BeforeFirst(wxT('\t')).c_str()));
      }
   }
#endif
   mCommandNameHash[name] = entry;

   if (entry->key != wxT("")) {
      mCommandKeyHash[entry->key] = entry;
   }

   return entry;
}

wxString CommandManager::GetLabel(const CommandListEntry *entry) const
{
   wxString label = entry->label;
   if (!entry->key.IsEmpty())
   {
      label += wxT("\t") + entry->key;
   }

   return label;
}

///Enables or disables a menu item based on its name (not the
///label in the menu bar, but the name of the command.)
///If you give it the name of a multi-item (one that was
///added using AddItemList(), it will enable or disable all
///of them at once
void CommandManager::Enable(CommandListEntry *entry, bool enabled)
{
   if (!entry->menu) {
      entry->enabled = enabled;
      return;
   }

   // LL:  Refresh from real state as we can get out of sync on the
   //      Mac due to its reluctance to enable menus when in a modal
   //      state.
   entry->enabled = entry->menu->IsEnabled(entry->id);

   // Only enabled if needed
   if (entry->enabled != enabled) {
      entry->menu->Enable(entry->id, enabled);
      entry->enabled = entry->menu->IsEnabled(entry->id);
   }

   if (entry->multi) {
      int i;
      int ID = entry->id;

      for(i=1; i<entry->count; i++) {
         ID = NextIdentifier(ID);

         // This menu item is not necessarily in the same menu, because
         // multi-items can be spread across multiple sub menus
         CommandListEntry *multiEntry = mCommandIDHash[ID];
         if (multiEntry) {
            wxMenuItem *item = multiEntry->menu->FindItem(ID);

         if (item) {
            item->Enable(enabled);
         } else {
            wxLogDebug(wxT("Warning: Menu entry with id %i in %s not found"),
                ID, (const wxChar*)entry->name);
         }
         } else {
            wxLogDebug(wxT("Warning: Menu entry with id %i not in hash"), ID);
         }
      }
   }
}

void CommandManager::Enable(wxString name, bool enabled)
{
   CommandListEntry *entry = mCommandNameHash[name];
   if (!entry || !entry->menu) {
      wxLogDebug(wxT("Warning: Unknown command enabled: '%s'"),
                 (const wxChar*)name);
      return;
   }

   Enable(entry, enabled);
}

void CommandManager::EnableUsingFlags(wxUint32 flags, wxUint32 mask)
{
   unsigned int i;

   for(i=0; i<mCommandList.GetCount(); i++) {
      CommandListEntry *entry = mCommandList[i];
      if (entry->multi && entry->index != 0)
         continue;

      wxUint32 combinedMask = (mask & entry->mask);
      if (combinedMask) {
         bool enable = ((flags & combinedMask) ==
                        (entry->flags & combinedMask));
         Enable(entry, enable);
      }
   }
}

bool CommandManager::GetEnabled(const wxString &name)
{
   CommandListEntry *entry = mCommandNameHash[name];
   if (!entry || !entry->menu) {
      wxLogDebug(wxT("Warning: command doesn't exist: '%s'"),
                 (const wxChar*)name);
      return false;
   }
   return entry->enabled;
}

void CommandManager::Check(wxString name, bool checked)
{
   CommandListEntry *entry = mCommandNameHash[name];
   if (!entry || !entry->menu) {
      return;
   }

   entry->menu->Check(entry->id, checked);
}

///Changes the label text of a menu item
void CommandManager::Modify(wxString name, wxString newLabel)
{
   CommandListEntry *entry = mCommandNameHash[name];
   if (entry && entry->menu) {
      entry->label = newLabel.BeforeFirst(wxT('\t'));
      entry->menu->SetLabel(entry->id, entry->label);
   }
}

void CommandManager::SetKeyFromName(wxString name, wxString key)
{
   CommandListEntry *entry = mCommandNameHash[name];
   if (entry) {
      entry->key = KeyStringNormalize(key);
   }
}

void CommandManager::SetKeyFromIndex(int i, wxString key)
{
   CommandListEntry *entry = mCommandList[i];
   entry->key = KeyStringNormalize(key);
}

void CommandManager::TellUserWhyDisallowed( wxUint32 flagsGot, wxUint32 flagsRequired )
{
   // The default string for 'reason' is a catch all.  I hope it won't ever be seen
   // and that we will get something more specific.
   wxString reason = _("There was a problem with your last action. If you think\nthis is a bug, please tell us exactly where it occurred.");

   wxUint32 missingFlags = flagsRequired & (~flagsGot );
   if( missingFlags & AudioIONotBusyFlag )
      reason= _("You can only do this when playing and recording are\n stopped. (Pausing is not sufficient.)");
   else if( missingFlags & StereoRequiredFlag )
      reason = _("You must first select some stereo audio for this\n to use. (You cannot use this with mono.)");
   else if( missingFlags & TimeSelectedFlag )
      reason = _("You must first select some audio for this to use.");
   else if( missingFlags & WaveTracksSelectedFlag)
      reason = _("You must first select some audio for this\n to use. (Selecting other kinds of track won't work.)");
   // If the only thing wrong was no tracks, we do nothing and don't report a problem
   else if( missingFlags == TracksExistFlag )
      return;

   wxMessageBox(reason, _("Disallowed"),  wxICON_WARNING | wxOK );
}

///
///
///
bool CommandManager::FilterKeyEvent(AudacityProject *project, wxKeyEvent & evt, bool permit)
{
   if (HandleMeta(evt))
   {
      return true;
   }

   // Any other keypresses must be destined for this project window.
   if (!permit && wxGetTopLevelParent(wxWindow::FindFocus()) != project)
   {
      return false;
   }

   wxUint32 flags = project->GetUpdateFlags();

   wxKeyEvent temp = evt;
   temp.SetEventType(wxEVT_KEY_DOWN);
   if (HandleKey(temp, flags, 0xFFFFFFFF))
   {
      temp.SetEventType(wxEVT_KEY_UP);
      HandleKey(temp, flags, 0xFFFFFFFF);

      return true;
   }

   return false;
}

/// HandleCommandEntry() takes a CommandListEntry and executes it
/// returning true iff successful.  If you pass any flags,
///the command won't be executed unless the flags are compatible
///with the command's flags.
bool CommandManager::HandleCommandEntry(CommandListEntry * entry, wxUint32 flags, wxUint32 mask, const wxEvent * evt)
{
   if (!entry || !entry->enabled)
      return false;

   wxUint32 combinedMask = (mask & entry->mask);
   if (combinedMask) {

      AudacityProject * proj;
      proj = GetActiveProject();
      wxASSERT( proj );
      if( !proj )
         return false;

      // NB: The call may have the side effect of changing flags.
      bool allowed = proj->TryToMakeActionAllowed( flags, entry->flags, combinedMask );
      if (!allowed)
      {
         TellUserWhyDisallowed(
            flags & combinedMask, entry->flags & combinedMask);
         return false;
      }
   }

   (*(entry->callback))(entry->index, evt);

   return true;
}

///Call this when a menu event is received.
///If it matches a command, it will call the appropriate
///CommandManagerListener function.  If you pass any flags,
///the command won't be executed unless the flags are compatible
///with the command's flags.
bool CommandManager::HandleMenuID(int id, wxUint32 flags, wxUint32 mask)
{
   CommandListEntry *entry = mCommandIDHash[id];
   return HandleCommandEntry( entry, flags, mask );
}

///Call this when a key event is received.
///If it matches a command, it will call the appropriate
///CommandManagerListener function.  If you pass any flags,
///the command won't be executed unless the flags are compatible
///with the command's flags.
bool CommandManager::HandleKey(wxKeyEvent &evt, wxUint32 flags, wxUint32 mask)
{
   wxString keyStr = KeyEventToKeyString(evt);
   CommandListEntry *entry = mCommandKeyHash[keyStr];

   if (entry)
   {
      if (evt.GetEventType() == wxEVT_KEY_DOWN && !entry->ignoredown)
      {
         return HandleCommandEntry( entry, flags, mask, &evt );
      }

      if (evt.GetEventType() == wxEVT_KEY_UP && (entry->wantevent || entry->ignoredown))
      {
         return HandleCommandEntry( entry, flags, mask, &evt );
      }
   }

   return false;
}

bool CommandManager::HandleMeta(wxKeyEvent &evt)
{
   wxString keyStr = KeyEventToKeyString(evt);
   CommandListEntry *entry = mCommandKeyHash[keyStr];

   // Return unhandle if it isn't a meta command
   if (!entry || !entry->isMeta)
   {
      return false;
   }

   // Meta commands are always disabled so they do not interfere with the
   // rest of the command handling.  But, to use the common handler, we
   // enable it temporarily and then disable it again after handling.
   entry->enabled = true;
   bool ret = HandleCommandEntry( entry, 0xffffffff, 0xffffffff, &evt );
   entry->enabled = false;

   return ret;
}

/// HandleTextualCommand() allows us a limitted version of script/batch
/// behavior, since we can get from a string command name to the actual
/// code to run.
bool CommandManager::HandleTextualCommand(wxString & Str, wxUint32 flags, wxUint32 mask)
{
   unsigned int i;

   // Linear search for now...
   for (i = 0; i < mCommandList.GetCount(); i++)
   {
      if (!mCommandList[i]->multi)
      {
         if( Str.IsSameAs( mCommandList[i]->name ))
         {
            return HandleCommandEntry( mCommandList[i], flags, mask);
         }
      }
   }
   // Not one of the singleton commands.
   // We could/should try all the list-style commands.
   // instead we only try the effects.
   AudacityProject * proj = GetActiveProject();
   if( !proj )
   {
      return false;
   }

   PluginManager & pm = PluginManager::Get();
   EffectManager & em = EffectManager::Get();
   const PluginDescriptor *plug = pm.GetFirstPlugin(PluginTypeEffect);
   while (plug)
   {
      if (em.GetEffectByIdentifier(plug->GetID()).IsSameAs(Str))
      {
         return proj->OnEffect(plug->GetID(), AudacityProject::OnEffectFlags::kConfigured);
      }
      plug = pm.GetNextPlugin(PluginTypeEffect);
   }

   return false;
}

void CommandManager::GetCategories(wxArrayString &cats)
{
   cats.Clear();

   size_t cnt = mCommandList.GetCount();

   for (size_t i = 0; i < cnt; i++) {
      wxString cat = mCommandList[i]->labelTop;
      if (cats.Index(cat) == wxNOT_FOUND) {
         cats.Add(cat);
      }
   }
#if 0
   mCommandList.GetCount(); i++) {
      if (includeMultis || !mCommandList[i]->multi)
         names.Add(mCommandList[i]->name);
   }

   AudacityProject *p = GetActiveProject();
   if (p == NULL) {
      return;
   }

   wxMenuBar *bar = p->GetMenuBar();
   size_t cnt = bar->GetMenuCount();
   for (size_t i = 0; i < cnt; i++) {
      cats.Add(bar->GetMenuLabelText(i));
   }

   cats.Add(COMMAND);
#endif
}

void CommandManager::GetAllCommandNames(wxArrayString &names,
                                        bool includeMultis)
{
   unsigned int i;

   for(i=0; i<mCommandList.GetCount(); i++) {
      if (!mCommandList[i]->multi)
         names.Add(mCommandList[i]->name);
      else if( includeMultis )
         names.Add(mCommandList[i]->name + wxT(":")/*+ mCommandList[i]->label*/);
   }
}

void CommandManager::GetAllCommandLabels(wxArrayString &names,
                                        bool includeMultis)
{
   unsigned int i;

   for(i=0; i<mCommandList.GetCount(); i++) {
      if (!mCommandList[i]->multi)
         names.Add(mCommandList[i]->label);
      else if( includeMultis )
         names.Add(mCommandList[i]->label);
   }
}

void CommandManager::GetAllCommandData(
   wxArrayString &names,
   wxArrayString &keys,
   wxArrayString &default_keys,
   wxArrayString &labels,
   wxArrayString &categories,
#if defined(EXPERIMENTAL_KEY_VIEW)
   wxArrayString &prefixes,
#endif
   bool includeMultis)
{
   unsigned int i;

   for(i=0; i<mCommandList.GetCount(); i++) {
      if (!mCommandList[i]->multi)
      {
         names.Add(mCommandList[i]->name);
         keys.Add(mCommandList[i]->key);
         default_keys.Add( mCommandList[i]->defaultKey);
         labels.Add(mCommandList[i]->label);
         categories.Add(mCommandList[i]->labelTop);
#if defined(EXPERIMENTAL_KEY_VIEW)
         prefixes.Add(mCommandList[i]->labelPrefix);
#endif
      }
      else if( includeMultis )
      {
         names.Add(mCommandList[i]->name);
         keys.Add(mCommandList[i]->key);
         default_keys.Add( mCommandList[i]->defaultKey);
         labels.Add(mCommandList[i]->label);
         categories.Add(mCommandList[i]->labelTop);
#if defined(EXPERIMENTAL_KEY_VIEW)
         prefixes.Add(mCommandList[i]->labelPrefix);
#endif
      }
   }
}
wxString CommandManager::GetLabelFromName(wxString name)
{
   CommandListEntry *entry = mCommandNameHash[name];
   if (!entry)
      return wxT("");

   return entry->label;
}

wxString CommandManager::GetPrefixedLabelFromName(wxString name)
{
   CommandListEntry *entry = mCommandNameHash[name];
   if (!entry)
      return wxT("");

#if defined(EXPERIMENTAL_KEY_VIEW)
   wxString prefix;
   if (!entry->labelPrefix.IsEmpty()) {
      prefix = entry->labelPrefix + wxT(" - ");
   }
   return wxMenuItem::GetLabelText(prefix + entry->label);
#else
   return wxString(entry->labelPrefix + wxT(" ") + entry->label).Trim(false).Trim(true);
#endif
}

wxString CommandManager::GetCategoryFromName(wxString name)
{
   CommandListEntry *entry = mCommandNameHash[name];
   if (!entry)
      return wxT("");

   return entry->labelTop;
}

wxString CommandManager::GetKeyFromName(wxString name)
{
   CommandListEntry *entry = mCommandNameHash[name];
   if (!entry)
      return wxT("");

   return entry->key;
}

wxString CommandManager::GetDefaultKeyFromName(wxString name)
{
   CommandListEntry *entry = mCommandNameHash[name];
   if (!entry)
      return wxT("");

   return entry->defaultKey;
}

bool CommandManager::HandleXMLTag(const wxChar *tag, const wxChar **attrs)
{
   if (!wxStrcmp(tag, wxT("audacitykeyboard"))) {
      mXMLKeysRead = 0;
   }

   if (!wxStrcmp(tag, wxT("command"))) {
      wxString name;
      wxString key;

      while(*attrs) {
         const wxChar *attr = *attrs++;
         const wxChar *value = *attrs++;

         if (!value)
            break;

         if (!wxStrcmp(attr, wxT("name")) && XMLValueChecker::IsGoodString(value))
            name = value;
         if (!wxStrcmp(attr, wxT("key")) && XMLValueChecker::IsGoodString(value))
            key = KeyStringNormalize(value);
      }

      if (mCommandNameHash[name]) {
         if (GetDefaultKeyFromName(name) != key) {
            mCommandNameHash[name]->key = KeyStringNormalize(key);
            mXMLKeysRead++;
         }
      }
   }

   return true;
}

void CommandManager::HandleXMLEndTag(const wxChar *tag)
{
   if (!wxStrcmp(tag, wxT("audacitykeyboard"))) {
      wxMessageBox(wxString::Format(_("Loaded %d keyboard shortcuts\n"),
                                    mXMLKeysRead),
                   _("Loading Keyboard Shortcuts"),
                   wxOK | wxCENTRE);
   }
}

XMLTagHandler *CommandManager::HandleXMLChild(const wxChar * WXUNUSED(tag))
{
   return this;
}

void CommandManager::WriteXML(XMLWriter &xmlFile)
{
   unsigned int j;

   xmlFile.StartTag(wxT("audacitykeyboard"));
   xmlFile.WriteAttr(wxT("audacityversion"), AUDACITY_VERSION_STRING);

   for(j=0; j<mCommandList.GetCount(); j++) {
      wxString label = mCommandList[j]->label;
      label = wxMenuItem::GetLabelText(label.BeforeFirst(wxT('\t')));

      xmlFile.StartTag(wxT("command"));
      xmlFile.WriteAttr(wxT("name"), mCommandList[j]->name);
      xmlFile.WriteAttr(wxT("label"), label);
      xmlFile.WriteAttr(wxT("key"), mCommandList[j]->key);
      xmlFile.EndTag(wxT("command"));
   }

   xmlFile.EndTag(wxT("audacitykeyboard"));
}

void CommandManager::SetDefaultFlags(wxUint32 flags, wxUint32 mask)
{
   mDefaultFlags = flags;
   mDefaultMask = mask;
}

void CommandManager::SetCommandFlags(wxString name,
                                     wxUint32 flags, wxUint32 mask)
{
   CommandListEntry *entry = mCommandNameHash[name];
   if (entry) {
      entry->flags = flags;
      entry->mask = mask;
   }
}

void CommandManager::SetCommandFlags(const wxChar **names,
                                     wxUint32 flags, wxUint32 mask)
{
   const wxChar **nptr = names;
   while(*nptr) {
      SetCommandFlags(wxString(*nptr), flags, mask);
      nptr++;
   }
}

void CommandManager::SetCommandFlags(wxUint32 flags, wxUint32 mask, ...)
{
   va_list list;
   va_start(list, mask);
   for(;;) {
      const wxChar *name = va_arg(list, const wxChar *);
      if (!name)
         break;
      SetCommandFlags(wxString(name), flags, mask);
   }
   va_end(list);
}

#if defined(__WXDEBUG__)
void CommandManager::CheckDups()
{
   int cnt = mCommandList.GetCount();
   for (size_t j = 0;  (int)j < cnt; j++) {
      if (mCommandList[j]->key.IsEmpty()) {
         continue;
      }

      if (mCommandList[j]->label.AfterLast(wxT('\t')) == wxT("allowdup")) {
         continue;
      }

      for (size_t i = 0; (int)i < cnt; i++) {
         if (i == j) {
            continue;
         }

         if (mCommandList[i]->key == mCommandList[j]->key) {
            wxString msg;
            msg.Printf(wxT("key combo '%s' assigned to '%s' and '%s'"),
                       mCommandList[i]->key.c_str(),
                       mCommandList[i]->label.BeforeFirst(wxT('\t')).c_str(),
                       mCommandList[j]->label.BeforeFirst(wxT('\t')).c_str());
            wxASSERT_MSG(mCommandList[i]->key != mCommandList[j]->key, msg);
         }
      }
   }
}

#endif
