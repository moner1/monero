// Copyright (c) 2017-2018, The Monero Project
// 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
// 
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

#include "device_ledger.hpp"
#include "log.hpp"
#include "ringct/rctOps.h"



namespace hw {

  namespace ledger {

  #ifdef WITH_DEVICE_LEDGER

    #undef MONERO_DEFAULT_LOG_CATEGORY
    #define MONERO_DEFAULT_LOG_CATEGORY "device.ledger"

    /* ===================================================================== */
    /* ===                           Debug                              ==== */
    /* ===================================================================== */
    void set_apdu_verbose(bool verbose) {
      apdu_verbose = verbose;
    }

    #define TRACKD MTRACE("hw")
    #define ASSERT_RV(rv)        CHECK_AND_ASSERT_THROW_MES((rv)==SCARD_S_SUCCESS, "Fail SCard API : (" << (rv) << ") "<< pcsc_stringify_error(rv)<<" Device="<<this->id<<", hCard="<<hCard<<", hContext="<<hContext);
    #define ASSERT_SW(sw,ok,msk) CHECK_AND_ASSERT_THROW_MES(((sw)&(mask))==(ok), "Wrong Device Status : SW=" << std::hex << (sw) << " (EXPECT=" << std::hex << (ok) << ", MASK=" << std::hex << (mask) << ")") ;
    #define ASSERT_T0(exp)       CHECK_AND_ASSERT_THROW_MES(exp, "Protocol assert failure: "#exp ) ;

    #ifdef DEBUG_HWDEVICE
      #define DEVICE_CONTROLE  :controle_device(hw::get_device("default"))
      crypto::secret_key viewkey;
      crypto::secret_key spendkey;
    #else
      #define DEVICE_CONTROLE 
    #endif

    /* ===================================================================== */
    /* ===                        Keymap                                ==== */
    /* ===================================================================== */

    ABPkeys::ABPkeys(const rct::key& A, const rct::key& B, size_t real_output_index, const rct::key& P, const rct::key& AK) {
      Aout = A;
      Bout = B;
      index = real_output_index;
      Pout = P;
      AKout = AK;
    }

    ABPkeys::ABPkeys(const ABPkeys& keys) {
      Aout  = keys.Aout;
      Bout  = keys.Bout;
      index = keys.index;
      Pout  = keys.Pout;
      AKout = keys.AKout;
    }

    bool Keymap::find(const rct::key& P, ABPkeys& keys) const {
      size_t sz = ABP.size();
      for (size_t i=0; i<sz; i++) {
        if (ABP[i].Pout == P) {
          keys = ABP[i];
          return true;
        }
      }
      return false;
    }

    void Keymap::add(const ABPkeys& keys) {
      ABP.push_back(keys);
    }

    void Keymap::clear() {
      ABP.clear();
    }

    #ifdef DEBUG_HWDEVICE
    void Keymap::log() {
      log_message("keymap", "content");
      size_t sz = ABP.size();
      for (size_t i=0; i<sz; i++) {
        log_message("  keymap", std::to_string(i));
        log_hexbuffer("    Aout", (char*)ABP[i].Aout.bytes, 32);
        log_hexbuffer("    Bout", (char*)ABP[i].Bout.bytes, 32);
        log_message  ("   index", std::to_string(ABP[i].index));
        log_hexbuffer("    Pout", (char*)ABP[i].Pout.bytes, 32);
      }
    }
    #endif

    /* ===================================================================== */
    /* ===                         Device                               ==== */
    /* ===================================================================== */

    static int device_id = 0;

    #define INS_NONE                            0x00
    #define INS_RESET                           0x02

    #define INS_GET_KEY                         0x20
    #define INS_PUT_KEY                         0x22
    #define INS_GET_CHACHA8_PREKEY              0x24
    #define INS_VERIFY_KEY                      0x26

    #define INS_SECRET_KEY_TO_PUBLIC_KEY        0x30
    #define INS_GEN_KEY_DERIVATION              0x32
    #define INS_DERIVATION_TO_SCALAR            0x34
    #define INS_DERIVE_PUBLIC_KEY               0x36
    #define INS_DERIVE_SECRET_KEY               0x38
    #define INS_GEN_KEY_IMAGE                   0x3A
    #define INS_SECRET_KEY_ADD                  0x3C
    #define INS_SECRET_KEY_SUB                  0x3E
    #define INS_GENERATE_KEYPAIR                0x40
    #define INS_SECRET_SCAL_MUL_KEY             0x42
    #define INS_SECRET_SCAL_MUL_BASE            0x44

    #define INS_DERIVE_SUBADDRESS_PUBLIC_KEY    0x46
    #define INS_GET_SUBADDRESS                  0x48
    #define INS_GET_SUBADDRESS_SPEND_PUBLIC_KEY 0x4A
    #define INS_GET_SUBADDRESS_SECRET_KEY       0x4C

    #define INS_OPEN_TX                         0x70
    #define INS_SET_SIGNATURE_MODE              0x72
    #define INS_GET_ADDITIONAL_KEY              0x74
    #define INS_STEALTH                         0x76
    #define INS_BLIND                           0x78
    #define INS_UNBLIND                         0x7A
    #define INS_VALIDATE                        0x7C
    #define INS_MLSAG                           0x7E
    #define INS_CLOSE_TX                        0x80


    #define INS_GET_RESPONSE                    0xc0


    void device_ledger::logCMD() {
      if (apdu_verbose) {
        char  strbuffer[1024];
        sprintf(strbuffer, "%.02x %.02x %.02x %.02x %.02x ",
          this->buffer_send[0],
          this->buffer_send[1],
          this->buffer_send[2],
          this->buffer_send[3],
          this->buffer_send[4]
          );
        buffer_to_str(strbuffer+strlen(strbuffer), sizeof(strbuffer), (char*)(this->buffer_send+5), this->length_send-5);
        MDEBUG( "CMD  :" << strbuffer);
      }
    }

    void device_ledger::logRESP() {
      if (apdu_verbose) {
        char  strbuffer[1024];
        sprintf(strbuffer, "%.02x%.02x ",
          this->buffer_recv[this->length_recv-2],
          this->buffer_recv[this->length_recv-1]
          );
        buffer_to_str(strbuffer+strlen(strbuffer), sizeof(strbuffer), (char*)(this->buffer_recv), this->length_recv-2);
        MDEBUG( "RESP :" << strbuffer);

      }
    }

    /* -------------------------------------------------------------- */
    device_ledger::device_ledger()  DEVICE_CONTROLE {
      this->id = device_id++;
      this->hCard   = 0;
      this->hContext = 0;
      this->reset_buffer();      
      MDEBUG( "Device "<<this->id <<" Created");
    }

    device_ledger::~device_ledger() {
      this->release();
      MDEBUG( "Device "<<this->id <<" Destroyed");
    }


    /* ======================================================================= */
    /*                                   MISC                                  */
    /* ======================================================================= */
    bool device_ledger::reset() {

      lock_device();
      try {
        int offset;
        reset_buffer();

        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_RESET;
        this->buffer_send[2] = 0x00;
        this->buffer_send[3] = 0x00;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = 0x00;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();
        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }
     
    unsigned int device_ledger::exchange(unsigned int ok, unsigned int mask) {
      LONG rv;
      int sw;

      ASSERT_T0(this->length_send <= BUFFER_SEND_SIZE);
      logCMD();
      this->length_recv = BUFFER_RECV_SIZE;
      rv = SCardTransmit(this->hCard,
                         SCARD_PCI_T0, this->buffer_send, this->length_send,
                         NULL,         this->buffer_recv, &this->length_recv);
      ASSERT_RV(rv);
      ASSERT_T0(this->length_recv <= BUFFER_RECV_SIZE);
      logRESP();

      sw = (this->buffer_recv[this->length_recv-2]<<8) | this->buffer_recv[this->length_recv-1];
      ASSERT_SW(sw,ok,msk);
      return sw;
    }

    void device_ledger::reset_buffer() {
      this->length_send = 0;
      memset(this->buffer_send, 0, BUFFER_SEND_SIZE);
      this->length_recv = 0;
      memset(this->buffer_recv, 0, BUFFER_RECV_SIZE);
    }

    void device_ledger::lock_device()   {
      MDEBUG( "Ask for LOCKING for device "<<this->id);
      device_locker.lock();
      MDEBUG( "Device "<<this->id << " LOCKed");
    }
    void device_ledger::unlock_device() {
      try {
        MDEBUG( "Ask for UNLOCKING for device "<<this->id);
      } catch (...) {
      }
      device_locker.unlock();
      MDEBUG( "Device "<<this->id << " UNLOCKed");
    }
    void device_ledger::lock_tx()       {
      MDEBUG( "Ask for LOCKING for TX "<<this->id);
      //tx_locker.lock();
      MDEBUG( "TX "<<this->id << " LOCKed");
    }
    void device_ledger::unlock_tx()     {
      MDEBUG( "Ask for UNLOCKING for TX "<<this->id);
      //tx_locker.unlock();
      MDEBUG( "TX "<<this->id << " UNLOCKed");
    }

    /* ======================================================================= */
    /*                              SETUP/TEARDOWN                             */
    /* ======================================================================= */

    bool device_ledger::set_name(const std::string & name) {
      this->name = name;
      return true;
    }

    const std::string device_ledger::get_name() const {
      if (this->full_name.empty() || (this->hCard == 0)) {
        return std::string("<disconnected:").append(this->name).append(">");
      }
      return this->full_name;
    }

    bool device_ledger::init(void) {
      LONG  rv;
      this->release();
      rv = SCardEstablishContext(SCARD_SCOPE_SYSTEM,0,0, &this->hContext);
      ASSERT_RV(rv);
      MDEBUG( "Device "<<this->id <<" SCardContext created: hContext="<<this->hContext);
      this->hCard = 0;
      return true;
    }

    bool device_ledger::release() {
      this->disconnect();
      if (this->hContext) {
        SCardReleaseContext(this->hContext);
        MDEBUG( "Device "<<this->id <<" SCardContext released: hContext="<<this->hContext);
        this->hContext = 0;
        this->full_name.clear();
      }
      return true;
    }

    bool device_ledger::connect(void) {
      BYTE  pbAtr[MAX_ATR_SIZE];
      LPSTR mszReaders;
      DWORD dwReaders;
      LONG  rv;
      DWORD dwState, dwProtocol, dwAtrLen, dwReaderLen;

      this->disconnect();
#ifdef SCARD_AUTOALLOCATE
      dwReaders = SCARD_AUTOALLOCATE;
      rv = SCardListReaders(this->hContext, NULL, (LPSTR)&mszReaders, &dwReaders);
#else
      dwReaders = 0;
      rv = SCardListReaders(this->hContext, NULL, NULL, &dwReaders);
      if (rv != SCARD_S_SUCCESS)
        return false;
      mszReaders = (LPSTR)calloc(dwReaders, sizeof(char));
      rv = SCardListReaders(this->hContext, NULL, mszReaders, &dwReaders);
#endif
      if (rv == SCARD_S_SUCCESS) {
        char* p;
        const char* prefix = this->name.c_str();

        p = mszReaders;
        MDEBUG( "Looking for " << std::string(prefix));
        while (*p) {
          MDEBUG( "Device Found: " <<  std::string(p));
          if ((strncmp(prefix, p, strlen(prefix))==0)) {
            MDEBUG( "Device Match: " <<  std::string(p));
            if ((rv = SCardConnect(this->hContext,
                                   p, SCARD_SHARE_EXCLUSIVE, SCARD_PROTOCOL_T0,
                                   &this->hCard, &dwProtocol))!=SCARD_S_SUCCESS) {
              break;
            }
            MDEBUG( "Device "<<this->id <<" Connected: hCard="<<this->hCard);
            dwAtrLen = sizeof(pbAtr);
            if ((rv = SCardStatus(this->hCard, NULL, &dwReaderLen, &dwState, &dwProtocol, pbAtr, &dwAtrLen))!=SCARD_S_SUCCESS) {
              break;
            }
            MDEBUG( "Device "<<this->id <<" Status OK");
            rv = SCARD_S_SUCCESS ;
            this->full_name = std::string(p);
            break;
          }
          p += strlen(p) +1;
        }
      }

      if (mszReaders) {
        #ifdef SCARD_AUTOALLOCATE
        SCardFreeMemory(this->hContext, mszReaders);
        #else
        free(mszReaders);
        #endif
        mszReaders = NULL;
      }
      if (rv != SCARD_S_SUCCESS) {
        if ( hCard) {
          SCardDisconnect(this->hCard, SCARD_UNPOWER_CARD);
          MDEBUG( "Device "<<this->id <<" disconnected: hCard="<<this->hCard);
          this->hCard = 0;
        }
      }
      ASSERT_RV(rv);

      this->reset();
      #ifdef DEBUG_HWDEVICE
      cryptonote::account_public_address pubkey;
      this->get_public_address(pubkey);
      crypto::secret_key vkey;
      crypto::secret_key skey;
      this->get_secret_keys(vkey,skey);
      #endif

      return rv==SCARD_S_SUCCESS;
    }

    bool device_ledger::disconnect() {
      if (this->hCard) {
        SCardDisconnect(this->hCard, SCARD_UNPOWER_CARD);
        MDEBUG( "Device "<<this->id <<" disconnected: hCard="<<this->hCard); 
        this->hCard = 0;
      }
      return true;
    }


    /* ======================================================================= */
    /*                             WALLET & ADDRESS                            */
    /* ======================================================================= */

    bool  device_ledger::get_secret_keys(crypto::secret_key &viewkey , crypto::secret_key &spendkey) {
        memset(viewkey.data, 0x00, 32);
        memset(spendkey.data, 0xFF, 32);
        return true;
    }

    /* Application API */
     bool device_ledger::get_public_address(cryptonote::account_public_address &pubkey){

      lock_device();
      try {
        int offset;
        reset_buffer();

        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_GET_KEY;
        this->buffer_send[2] = 0x01;
        this->buffer_send[3] = 0x00;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = 0x00;
        offset += 1;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();

        memmove(pubkey.m_view_public_key.data, this->buffer_recv, 32);
        memmove(pubkey.m_spend_public_key.data, this->buffer_recv+32, 32);
        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }

    #ifdef DEBUG_HWDEVICE
    bool  device_ledger::get_secret_keys(crypto::secret_key &viewkey , crypto::secret_key &spendkey) {
      lock_device();
      try {
        //spcialkey, normal conf handled in decrypt
        int offset;
        reset_buffer();

        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_GET_KEY;
        this->buffer_send[2] = 0x02;
        this->buffer_send[3] = 0x00;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = 0x00;
        offset += 1;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();

        //clear key
        memmove(ledger::viewkey.data,  this->buffer_recv+64, 32);
        memmove(ledger::spendkey.data, this->buffer_recv+96, 32);
        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }
    #endif

    bool  device_ledger::generate_chacha_key(const cryptonote::account_keys &keys, crypto::chacha_key &key) {
      lock_device();
      try {
        int offset;

        #ifdef DEBUG_HWDEVICE
        const cryptonote::account_keys keys_x = decrypt(keys);
        crypto::chacha_key key_x;
        this->controle_device.generate_chacha_key(keys_x, key_x);
        #endif

        reset_buffer();
        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_GET_CHACHA8_PREKEY;
        this->buffer_send[2] = 0x00;
        this->buffer_send[3] = 0x00;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = 0x00;
        offset += 1;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();

        char prekey[200];
        memmove(prekey, &this->buffer_recv[0], 200);
        crypto::generate_chacha_key(&prekey[0], sizeof(prekey), key, true);

        #ifdef DEBUG_HWDEVICE
        hw::ledger::check32("generate_chacha_key", "key", (char*)key_x.data(), (char*)key.data());
        #endif

        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }

    /* ======================================================================= */
    /*                               SUB ADDRESS                               */
    /* ======================================================================= */

    bool device_ledger::derive_subaddress_public_key(const crypto::public_key &pub, const crypto::key_derivation &derivation, const std::size_t output_index, crypto::public_key &derived_pub){

      lock_device();
      try {
        int offset =0;
        unsigned char options = 0;

        #ifdef DEBUG_HWDEVICE
        const crypto::public_key pub_x = pub;
        const crypto::key_derivation derivation_x  = hw::ledger::decrypt(derivation);
        const std::size_t output_index_x = output_index;
        crypto::public_key derived_pub_x;
        this->controle_device.derive_subaddress_public_key(pub_x, derivation_x,output_index_x,derived_pub_x);
        #endif

        reset_buffer();

        options = 0;

        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_DERIVE_SUBADDRESS_PUBLIC_KEY;
        this->buffer_send[2] = 0x00;
        this->buffer_send[3] = 0x00;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = options;
        offset += 1;
        //pub
        memmove(this->buffer_send+offset, pub.data, 32);
        offset += 32;
        //derivation
        memmove(this->buffer_send+offset, derivation.data, 32);
        offset += 32;
        //index
        this->buffer_send[offset+0] = output_index>>24;
        this->buffer_send[offset+1] = output_index>>16;
        this->buffer_send[offset+2] = output_index>>8;
        this->buffer_send[offset+3] = output_index>>0;
        offset += 4;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();

        //pub key
        memmove(derived_pub.data, &this->buffer_recv[0], 32);

        #ifdef DEBUG_HWDEVICE
        hw::ledger::check32("derive_subaddress_public_key", "derived_pub", derived_pub_x.data, derived_pub.data);
        #endif

        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }

    bool device_ledger::get_subaddress_spend_public_key(const cryptonote::account_keys& keys, const cryptonote::subaddress_index &index, crypto::public_key &D) {
      lock_device();
      try {
        int offset =0;
        unsigned char options = 0;

        #ifdef DEBUG_HWDEVICE
        const cryptonote::account_keys     keys_x =  hw::ledger::decrypt(keys);
        const cryptonote::subaddress_index index_x = index;
        crypto::public_key                 D_x;
        this->controle_device.get_subaddress_spend_public_key(keys_x, index_x, D_x);
        #endif

        if (index.is_zero()) {
           D = keys.m_account_address.m_spend_public_key;
        } else {

          reset_buffer();

          this->buffer_send[0] = 0x00;
          this->buffer_send[1] = INS_GET_SUBADDRESS_SPEND_PUBLIC_KEY;
          this->buffer_send[2] = 0x00;
          this->buffer_send[3] = 0x00;
          this->buffer_send[4] = 0x00;
          offset = 5;
          //options
          this->buffer_send[offset] = 0x00;
          offset += 1;
          //index
          static_assert(sizeof(cryptonote::subaddress_index) == 8, "cryptonote::subaddress_index shall be 8 bytes length");
          memmove(this->buffer_send+offset, &index, sizeof(cryptonote::subaddress_index));
          offset +=8 ;

          this->buffer_send[4] = offset-5;
          this->length_send = offset;
          this->exchange();

          memmove(D.data, &this->buffer_recv[0], 32);
        }

        #ifdef DEBUG_HWDEVICE
        hw::ledger::check32("get_subaddress_spend_public_key", "D", D_x.data, D.data);
        #endif

        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }

    bool  device_ledger::get_subaddress_spend_public_keys(const cryptonote::account_keys &keys, uint32_t account, uint32_t begin, uint32_t end, std::vector<crypto::public_key> &pkeys) {
     cryptonote::subaddress_index index = {account, begin};
     crypto::public_key D;
     for (uint32_t idx = begin; idx < end; ++idx) {
        index.minor = idx;
        this->get_subaddress_spend_public_key(keys, index, D);
        pkeys.push_back(D);
      }
      return true;
    }

    bool device_ledger::get_subaddress(const cryptonote::account_keys& keys, const cryptonote::subaddress_index &index, cryptonote::account_public_address &address) {
      lock_device();
      try {
        int offset =0;
        unsigned char options = 0;

        #ifdef DEBUG_HWDEVICE
        const cryptonote::account_keys            keys_x =  hw::ledger::decrypt(keys);
        const cryptonote::subaddress_index        index_x = index;
        cryptonote::account_public_address  address_x;
        this->controle_device.get_subaddress(keys_x, index_x, address_x);
        #endif

        if (index.is_zero()) {
          address = keys.m_account_address;
        } else {
          reset_buffer();

          this->buffer_send[0] = 0x00;
          this->buffer_send[1] = INS_GET_SUBADDRESS;
          this->buffer_send[2] = 0x00;
          this->buffer_send[3] = 0x00;
          this->buffer_send[4] = 0x00;
          offset = 5;
          //options
          this->buffer_send[offset] = 0x00;
          offset += 1;
          //index
          static_assert(sizeof(cryptonote::subaddress_index) == 8, "cryptonote::subaddress_index shall be 8 bytes length");
          memmove(this->buffer_send+offset, &index, sizeof(cryptonote::subaddress_index));
          offset +=8 ;

          this->buffer_send[4] = offset-5;
          this->length_send = offset;
          this->exchange();

          memmove(address.m_view_public_key.data,  &this->buffer_recv[0],  32);
          memmove(address.m_spend_public_key.data, &this->buffer_recv[32], 32);
        }

        #ifdef DEBUG_HWDEVICE
        hw::ledger::check32("get_subaddress", "address.m_view_public_key.data", address_x.m_view_public_key.data, address.m_view_public_key.data);
        hw::ledger::check32("get_subaddress", "address.m_spend_public_key.data", address_x.m_spend_public_key.data, address.m_spend_public_key.data);
        #endif

        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }

    bool  device_ledger::get_subaddress_secret_key(const crypto::secret_key &sec, const cryptonote::subaddress_index &index, crypto::secret_key &sub_sec) {
      lock_device();
      try {
        int offset =0;
        unsigned char options = 0;

        #ifdef DEBUG_HWDEVICE
        const crypto::secret_key            sec_x =  hw::ledger::decrypt(sec);
        const cryptonote::subaddress_index  index_x = index;
        crypto::secret_key            sub_sec_x;
        this->controle_device.get_subaddress_secret_key(sec_x, index_x, sub_sec_x);
        #endif

        reset_buffer();

        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_GET_SUBADDRESS_SECRET_KEY;
        this->buffer_send[2] = 0x00;
        this->buffer_send[3] = 0x00;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = options;
        offset += 1;
        //sec
        memmove(this->buffer_send+offset, sec.data, 32);
        offset += 32;
        //index
        static_assert(sizeof(cryptonote::subaddress_index) == 8, "cryptonote::subaddress_index shall be 8 bytes length");
        memmove(this->buffer_send+offset, &index, sizeof(cryptonote::subaddress_index));
        offset +=8 ;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();

        memmove(sub_sec.data,  &this->buffer_recv[0],  32);

        #ifdef DEBUG_HWDEVICE
        crypto::secret_key            sub_sec_clear =   hw::ledger::decrypt(sub_sec);
        hw::ledger::check32("get_subaddress_secret_key", "sub_sec", sub_sec_x.data, sub_sec_clear.data);
        #endif

        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }

    /* ======================================================================= */
    /*                            DERIVATION & KEY                             */
    /* ======================================================================= */

    bool  device_ledger::verify_keys(const crypto::secret_key &secret_key, const crypto::public_key &public_key) {
      lock_device();
      try {
        int offset =0,sw;
        unsigned char options = 0;
        reset_buffer();
        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_VERIFY_KEY;
        this->buffer_send[2] = 0x00;
        this->buffer_send[3] = 0x00;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = 0x00;
        offset += 1;
        //sec
        memmove(this->buffer_send+offset, secret_key.data, 32);
        offset += 32;
        //pub
        memmove(this->buffer_send+offset, public_key.data, 32);
        offset += 32;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();

        uint32_t verified =
          this->buffer_recv[0] << 24 |
          this->buffer_recv[1] << 16 |
          this->buffer_recv[2] << 8  |
          this->buffer_recv[3] << 0  ;

        unlock_device();
        return verified == 1;
      }catch (...)  {
        unlock_device();
        throw;
      }
      return false;
    }

    bool device_ledger::scalarmultKey(rct::key & aP, const rct::key &P, const rct::key &a) {
      lock_device();
      try {
        int offset =0;
        unsigned char options = 0;

        #ifdef DEBUG_HWDEVICE
        const rct::key pub_x    =  pub;
        const rct::key sec_x    =  hw::ledger::decrypt(sec);
        rct::key mulkey_x;
        this->controle_device.scalarmultKey(pub_x, sec_x, mulkey_x);
        #endif

        reset_buffer();

        options = 0;

        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_SECRET_SCAL_MUL_KEY;
        this->buffer_send[2] = 0x00;
        this->buffer_send[3] = 0x00;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = options;
        offset += 1;
        //pub
        memmove(this->buffer_send+offset, P.bytes, 32);
        offset += 32;
        //sec
        memmove(this->buffer_send+offset, a.bytes, 32);
        offset += 32;


        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();

        //pub key
        memmove(aP.bytes, &this->buffer_recv[0], 32);

        #ifdef DEBUG_HWDEVICE
        hw::ledger::check32("scalarmultKey", "mulkey", (char*)mulkey_x.bytes, (char*)mulkey.bytes);
        #endif

        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }

    bool device_ledger::scalarmultBase(rct::key &aG, const rct::key &a) {
      lock_device();
      try {
        int offset =0;
        unsigned char options = 0;

        #ifdef DEBUG_HWDEVICE
        const rct::key sec_x    =  hw::ledger::decrypt(sec);
        rct::key mulkey_x;
        this->controle_device.scalarmultBase(sec_x, mulkey_x);
        #endif

        reset_buffer();

        options = 0;

        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_SECRET_SCAL_MUL_BASE;
        this->buffer_send[2] = 0x00;
        this->buffer_send[3] = 0x00;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = options;
        offset += 1;
        //sec
        memmove(this->buffer_send+offset, a.bytes, 32);
        offset += 32;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();

        //pub key
        memmove(aG.bytes, &this->buffer_recv[0], 32);

        #ifdef DEBUG_HWDEVICE
        hw::ledger::check32("scalarmultBase", "mulkey", (char*)mulkey_x.bytes, (char*)mulkey.bytes);
        #endif

        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }

    bool device_ledger::sc_secret_add( crypto::secret_key &r, const crypto::secret_key &a, const crypto::secret_key &b) {

      lock_device();
      try {
        int offset =0;
        unsigned char options = 0;

        #ifdef DEBUG_HWDEVICE
        const crypto::secret_key a_x = hw::ledger::decrypt(a);
        const crypto::secret_key b_x = hw::ledger::decrypt(b);
        crypto::secret_key r_x;
        this->controle_device.sc_secret_add(r_x, a_x, b_x);
        #endif

        reset_buffer();

        options = 0;

        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_SECRET_KEY_ADD;
        this->buffer_send[2] = 0x00;
        this->buffer_send[3] = 0x00;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = options;
        offset += 1;
        //sec key
        memmove(this->buffer_send+offset, a.data, 32);
        offset += 32;
        //sec key
        memmove(this->buffer_send+offset, b.data, 32);
        offset += 32;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();

        //pub key
        memmove(r.data, &this->buffer_recv[0], 32);

        #ifdef DEBUG_HWDEVICE
        crypto::secret_key r_clear = hw::ledger::decrypt(r);
        hw::ledger::check32("sc_secret_add", "r", r_x.data, r_clear.data);
        #endif

        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }

    bool  device_ledger::generate_keys(crypto::public_key &pub, crypto::secret_key &sec, const crypto::secret_key& recovery_key, bool recover, crypto::secret_key &rng) {
      if (recover) {
        throw std::runtime_error("device generate key does not support recover");
      }

      lock_device();
      try {
        int offset =0;
        unsigned char options = 0;

        #ifdef DEBUG_HWDEVICE
        bool recover_x = recover;
        const crypto::secret_key recovery_key_x = recovery_key;
        crypto::public_key pub_x;
        crypto::secret_key sec_x;
        #endif

        reset_buffer();

        options = 0;

        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_GENERATE_KEYPAIR;
        this->buffer_send[2] = 0x00;
        this->buffer_send[3] = 0x00;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = options;
        offset += 1;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();

        //pub key
        memmove(pub.data, &this->buffer_recv[0], 32);
        memmove(sec.data, &this->buffer_recv[32], 32);

        #ifdef DEBUG_HWDEVICE
        crypto::secret_key sec_clear = hw::ledger::decrypt(sec);
        sec_x = sec_clear;
        crypto::secret_key_to_public_key(sec_x,pub_x);
        hw::ledger::check32("generate_keys", "pub", pub_x.data, pub.data);
        #endif

        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;

    }

    bool device_ledger::generate_key_derivation(const crypto::public_key &pub, const crypto::secret_key &sec, crypto::key_derivation &derivation) {
      lock_device();
      try {
        int offset =0;
        unsigned char options = 0;

        #ifdef DEBUG_HWDEVICE
        const crypto::public_key pub_x = pub;
        const crypto::secret_key sec_x = hw::ledger::decrypt(sec);
        crypto::key_derivation derivation_x;
        this->controle_device.generate_key_derivation(pub_x, sec_x, derivation_x);
        hw::ledger::log_hexbuffer("generate_key_derivation: sec_x.data", sec_x.data, 32);
        hw::ledger::log_hexbuffer("generate_key_derivation: pub_x.data", pub_x.data, 32);
        hw::ledger::log_hexbuffer("generate_key_derivation: derivation_x.data", derivation_x.data, 32);
        #endif

        reset_buffer();

        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_GEN_KEY_DERIVATION;
        this->buffer_send[2] = 0x00;
        this->buffer_send[3] = 0x00;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = options;
        offset += 1;
        //pub
        memmove(this->buffer_send+offset, pub.data, 32);
        offset += 32;
         //sec
        memmove(this->buffer_send+offset, sec.data, 32);
        offset += 32;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();

        //derivattion data
        memmove(derivation.data, &this->buffer_recv[0], 32);

        #ifdef DEBUG_HWDEVICE
        crypto::key_derivation derivation_clear  = hw::ledger::decrypt(derivation);
        hw::ledger::check32("generate_key_derivation", "derivation", derivation_x.data, derivation_clear.data);
        #endif

        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }

    bool device_ledger::derivation_to_scalar(const crypto::key_derivation &derivation, const size_t output_index, crypto::ec_scalar &res) {
      lock_device();
      try {
        int offset;
        unsigned char options;

        #ifdef DEBUG_HWDEVICE
        const crypto::key_derivation derivation_x = hw::ledger::decrypt(derivation);
        const size_t output_index_x               = output_index;
        crypto::ec_scalar res_x;
        this->controle_device.derivation_to_scalar(derivation_x, output_index_x, res_x);
        #endif

        reset_buffer();

        options = 0;

        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_DERIVATION_TO_SCALAR;
        this->buffer_send[2] = 0x00;
        this->buffer_send[3] = 0x00;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = options;
        offset += 1;
        //derivattion
        memmove(this->buffer_send+offset, derivation.data, 32);
        offset += 32;
        //index
        this->buffer_send[offset+0] = output_index>>24;
        this->buffer_send[offset+1] = output_index>>16;
        this->buffer_send[offset+2] = output_index>>8;
        this->buffer_send[offset+3] = output_index>>0;
        offset += 4;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();

        //derivattion data
        memmove(res.data, &this->buffer_recv[0], 32);

        #ifdef DEBUG_HWDEVICE
        crypto::ec_scalar res_clear  = hw::ledger::decrypt(res);
        hw::ledger::check32("derivation_to_scalar", "res", res_x.data, res_clear.data);
        #endif

        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }

    bool device_ledger::derive_secret_key(const crypto::key_derivation &derivation, const std::size_t output_index, const crypto::secret_key &sec, crypto::secret_key &derived_sec) {
      lock_device();
      try {
        int offset;
        unsigned char options;

        #ifdef DEBUG_HWDEVICE
        const crypto::key_derivation derivation_x   = hw::ledger::decrypt(derivation);
        const std::size_t            output_index_x = output_index;
        const crypto::secret_key     sec_x          = hw::ledger::decrypt(sec);
        crypto::secret_key           derived_sec_x;
        this->controle_device.derive_secret_key(derivation_x, output_index_x, sec_x, derived_sec_x);
        #endif

        reset_buffer();

        options = 0;

        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_DERIVE_SECRET_KEY;
        this->buffer_send[2] = 0x00;
        this->buffer_send[3] = 0x00;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = options;
        offset += 1;
        //derivation
        memmove(this->buffer_send+offset, derivation.data, 32);
        offset += 32;
        //index
        this->buffer_send[offset+0] = output_index>>24;
        this->buffer_send[offset+1] = output_index>>16;
        this->buffer_send[offset+2] = output_index>>8;
        this->buffer_send[offset+3] = output_index>>0;
        offset += 4;
        //sec
        memmove(this->buffer_send+offset, sec.data, 32);
        offset += 32;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();

        //pub key
        memmove(derived_sec.data, &this->buffer_recv[0], 32);

        #ifdef DEBUG_HWDEVICE
        crypto::secret_key derived_sec_clear = hw::ledger::decrypt(derived_sec);
        hw::ledger::check32("derive_secret_key", "derived_sec", derived_sec_x.data, derived_sec_clear.data);
        #endif

        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }

    bool device_ledger::derive_public_key(const crypto::key_derivation &derivation, const std::size_t output_index, const crypto::public_key &pub, crypto::public_key &derived_pub){
      lock_device();
      try {
        int offset;
        unsigned char options;

        #ifdef DEBUG_HWDEVICE
        const crypto::key_derivation derivation_x   = hw::ledger::decrypt(derivation);
        const std::size_t            output_index_x = output_index;
        const crypto::public_key     pub_x        = pub;
        crypto::public_key           derived_pub_x;
        this->controle_device.derive_public_key(derivation_x, output_index_x, pub_x, derived_pub_x);
        #endif

        reset_buffer();

        options = 0;

        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_DERIVE_PUBLIC_KEY;
        this->buffer_send[2] = 0x00;
        this->buffer_send[3] = 0x00;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = options;
        offset += 1;
        //derivation
        memmove(this->buffer_send+offset, derivation.data, 32);
        offset += 32;
        //index
        this->buffer_send[offset+0] = output_index>>24;
        this->buffer_send[offset+1] = output_index>>16;
        this->buffer_send[offset+2] = output_index>>8;
        this->buffer_send[offset+3] = output_index>>0;
        offset += 4;
        //pub
        memmove(this->buffer_send+offset, pub.data, 32);
        offset += 32;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();

        //pub key
        memmove(derived_pub.data, &this->buffer_recv[0], 32);

        #ifdef DEBUG_HWDEVICE
        hw::ledger::check32("derive_public_key", "derived_pub", derived_pub_x.data, derived_pub.data);
        #endif

        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }

    bool device_ledger::secret_key_to_public_key(const crypto::secret_key &sec, crypto::public_key &pub) {
      lock_device();
      try {
        int offset;
        unsigned char options;

        #ifdef DEBUG_HWDEVICE
        const crypto::secret_key sec_x = hw::ledger::decrypt(sec);
        crypto::public_key       pub_x;
        this->controle_device.secret_key_to_public_key(sec_x, pub_x);
        #endif

        reset_buffer();

        options = 0;

        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_SECRET_KEY_TO_PUBLIC_KEY;
        this->buffer_send[2] = 0x00;
        this->buffer_send[3] = 0x00;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = options;
        offset += 1;
        //sec key
        memmove(this->buffer_send+offset, sec.data, 32);
        offset += 32;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();

        //pub key
        memmove(pub.data, &this->buffer_recv[0], 32);

        #ifdef DEBUG_HWDEVICE
        hw::ledger::check32("secret_key_to_public_key", "pub", pub_x.data, pub.data);
        #endif

        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }

    bool device_ledger::generate_key_image(const crypto::public_key &pub, const crypto::secret_key &sec, crypto::key_image &image){
      lock_device();
      try {
        int offset;
        unsigned char options;

        #ifdef DEBUG_HWDEVICE
        const crypto::public_key pub_x = pub;
        const crypto::secret_key sec_x = hw::ledger::decrypt(sec);
        crypto::key_image        image_x;
        this->controle_device.generate_key_image(pub_x, sec_x, image_x);
        #endif

        reset_buffer();

        options = 0;

        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_GEN_KEY_IMAGE;
        this->buffer_send[2] = 0x00;
        this->buffer_send[3] = 0x00;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = options;
        offset += 1;
        //pub
        memmove(this->buffer_send+offset, pub.data, 32);
        offset += 32;
        //sec
        memmove(this->buffer_send+offset, sec.data, 32);
        offset += 32;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();

        //pub key
        memmove(image.data, &this->buffer_recv[0], 32);

        #ifdef DEBUG_HWDEVICE
        hw::ledger::check32("generate_key_image", "image", image_x.data, image.data);
        #endif

        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }

    /* ======================================================================= */
    /*                               TRANSACTION                               */
    /* ======================================================================= */

    bool device_ledger::open_tx(crypto::secret_key &tx_key) {
      lock_device();
      try {
        int offset =0;
        unsigned char options = 0;

        lock_tx();
        reset_buffer();
        key_map.clear();

        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_OPEN_TX;
        this->buffer_send[2] = 0x01;
        this->buffer_send[3] = 0x00;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = 0x00;
        offset += 1;
        //account
        this->buffer_send[offset+0] = 0x00;
        this->buffer_send[offset+1] = 0x00;
        this->buffer_send[offset+2] = 0x00;
        this->buffer_send[offset+3] = 0x00;
        offset += 4;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();

        memmove(tx_key.data, &this->buffer_recv[32], 32);
        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }

    bool  device_ledger::set_signature_mode(unsigned int sig_mode) {
      lock_device();
      try {
        int offset =0;
        unsigned char options = 0;

        reset_buffer();

        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_SET_SIGNATURE_MODE;
        this->buffer_send[2] = 0x01;
        this->buffer_send[3] = 0x00;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = 0x00;
        offset += 1;
        //account
        this->buffer_send[offset] = sig_mode;
        offset += 1;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();
        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }

    bool device_ledger::encrypt_payment_id(const crypto::public_key &public_key, const crypto::secret_key &secret_key, crypto::hash8 &payment_id) {
      lock_device();
      try {
        int offset =0;
        unsigned char options = 0;

        #ifdef DEBUG_HWDEVICE
        const crypto::public_key public_key_x = public_key;
        const crypto::secret_key secret_key_x = hw::ledger::decrypt(secret_key);
        crypto::hash8 payment_id_x = payment_id;
        this->controle_device.encrypt_payment_id(public_key_x, secret_key_x, payment_id_x);
        #endif

        reset_buffer();

        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_STEALTH;
        this->buffer_send[2] = 0x00;
        this->buffer_send[3] = 0x00;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = 0x00;
        offset += 1;
        //pub
        memmove(&this->buffer_send[offset], public_key.data, 32);
        offset += 32;
        //sec
        memmove(&this->buffer_send[offset], secret_key.data, 32);
        offset += 32;
        //id
        memmove(&this->buffer_send[offset], payment_id.data, 8);
        offset += 8;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();
        memmove(payment_id.data, &this->buffer_recv[0], 8);

        #ifdef DEBUG_HWDEVICE
        hw::ledger::check8("stealth", "payment_id", payment_id_x.data, payment_id.data);
        #endif

        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }

    bool device_ledger::add_output_key_mapping(const crypto::public_key &Aout, const crypto::public_key &Bout, size_t real_output_index,
                                        const rct::key &amount_key,  const crypto::public_key &out_eph_public_key)  {
      lock_device();
      try {
        key_map.add(ABPkeys(rct::pk2rct(Aout),rct::pk2rct(Bout), real_output_index, rct::pk2rct(out_eph_public_key), amount_key));
        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }

    bool  device_ledger::ecdhEncode(rct::ecdhTuple & unmasked, const rct::key & AKout) {
      lock_device();
      try {
        int offset =0;
        unsigned char options = 0;

        #ifdef DEBUG_HWDEVICE
        const rct::key AKout_x =   hw::ledger::decrypt(AKout);
        rct::ecdhTuple unmasked_x = unmasked;
        this->controle_device.ecdhEncode(AKout_x, unmasked_x);
        #endif

        reset_buffer();

        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_BLIND;
        this->buffer_send[2] = 0x00;
        this->buffer_send[3] = 0x00;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = 0x00;
        offset += 1;
        // AKout
        memmove(this->buffer_send+offset, AKout.bytes, 32);
        offset += 32;
        //mask k
        memmove(this->buffer_send+offset, unmasked.mask.bytes, 32);
        offset += 32;
        //value v
        memmove(this->buffer_send+offset, unmasked.amount.bytes, 32);
        offset += 32;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();

        memmove(unmasked.amount.bytes, &this->buffer_recv[0],  32);
        memmove(unmasked.mask.bytes,  &this->buffer_recv[32], 32);

        #ifdef DEBUG_HWDEVICE
        hw::ledger::check32("ecdhEncode", "amount", (char*)unmasked_x.amount.bytes, (char*)unmasked.amount.bytes);
        hw::ledger::check32("ecdhEncode", "mask", (char*)unmasked_x.mask.bytes, (char*)unmasked.mask.bytes);

        hw::ledger::log_hexbuffer("Blind AKV input", (char*)&this->buffer_recv[64], 3*32);
        #endif

        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }

    bool  device_ledger::ecdhDecode(rct::ecdhTuple & masked, const rct::key & AKout) {
      lock_device();
      try {
        int offset =0;
        unsigned char options = 0;

        #ifdef DEBUG_HWDEVICE
        const rct::key AKout_x =   hw::ledger::decrypt(AKout);
        rct::ecdhTuple masked_x = masked;
        this->controle_device.ecdhDecode(AKout_x, masked_x);
        #endif

        reset_buffer();

        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_UNBLIND;
        this->buffer_send[2] = 0x00;
        this->buffer_send[3] = 0x00;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = 0x00;
        offset += 1;
        // AKout
        memmove(this->buffer_send+offset, AKout.bytes, 32);
        offset += 32;
        //mask k
        memmove(this->buffer_send+offset, masked.mask.bytes, 32);
        offset += 32;
        //value v
        memmove(this->buffer_send+offset, masked.amount.bytes, 32);
        offset += 32;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();

        memmove(masked.amount.bytes, &this->buffer_recv[0],  32);
        memmove(masked.mask.bytes,  &this->buffer_recv[32], 32);

        #ifdef DEBUG_HWDEVICE
        hw::ledger::check32("ecdhDecode", "amount", (char*)masked_x.amount.bytes, (char*)masked.amount.bytes);
        hw::ledger::check32("ecdhDecode", "mask", (char*)masked_x.mask.bytes,(char*) masked.mask.bytes);
        #endif

        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }

    bool device_ledger::mlsag_prehash(const std::string &blob, size_t inputs_size, size_t outputs_size,
                                     const rct::keyV &hashes, const rct::ctkeyV &outPk,
                                     rct::key &prehash) {

      lock_device();
      try {
        unsigned char options = 0;
        unsigned int  data_offset, C_offset, kv_offset, i;
        const char *data;

        #ifdef DEBUG_HWDEVICE
        const std::string blob_x  = blob;
        size_t inputs_size_x      = inputs_size;
        size_t outputs_size_x     = outputs_size;
        const rct::keyV hashes_x  = hashes;
        const rct::ctkeyV outPk_x = outPk;
        rct::key prehash_x;
        this->controle_device.mlsag_prehash(blob_x, inputs_size_x, outputs_size_x, hashes_x, outPk_x, prehash_x);
        if (inputs_size) {
          log_message("mlsag_prehash", (std::string("inputs_size not null: ") +  std::to_string(inputs_size)).c_str());
        }
        this->key_map.log();
        #endif

        data = blob.data();

        // ======  u8 type, varint txnfee ======
        int offset;

        reset_buffer();

        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_VALIDATE;
        this->buffer_send[2] = 0x01;
        this->buffer_send[3] = 0x01;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = (inputs_size == 0)?0x00:0x80;
        offset += 1;

        //type
        this->buffer_send[offset] = data[0];
        offset += 1;

        //txnfee
        data_offset = 1;
        while (data[data_offset]&0x80) {
          this->buffer_send[offset] = data[data_offset];
          offset += 1;
          data_offset += 1;
        }
        this->buffer_send[offset] = data[data_offset];
        offset += 1;
        data_offset += 1;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();

        //pseudoOuts
        for ( i = 0; i < inputs_size; i++) {
          reset_buffer();
          this->buffer_send[0] = 0x00;
          this->buffer_send[1] = INS_VALIDATE;
          this->buffer_send[2] = 0x01;
          this->buffer_send[3] = i+2;
          this->buffer_send[4] = 0x00;
          offset = 5;
          //options
          this->buffer_send[offset] = (i==inputs_size-1)? 0x00:0x80;
          offset += 1;
          //pseudoOut
          memmove(this->buffer_send+offset, data+data_offset,32);
          offset += 32;
          data_offset += 32;

          this->buffer_send[4] = offset-5;
          this->length_send = offset;
          this->exchange();
        }

        // ======  Aout, Bout, AKout, C, v, k ======
        kv_offset = data_offset;
        C_offset = kv_offset+ (32*2)*outputs_size;
        for ( i = 0; i < outputs_size; i++) {
          ABPkeys outKeys;
          bool found;

          found = this->key_map.find(outPk[i].dest, outKeys);
          if (!found) {
            log_hexbuffer("Pout not found", (char*)outPk[i].dest.bytes, 32);
            CHECK_AND_ASSERT_THROW_MES(found, "Pout not found");
          }
          reset_buffer();

          this->buffer_send[0] = 0x00;
          this->buffer_send[1] = INS_VALIDATE;
          this->buffer_send[2] = 0x02;
          this->buffer_send[3] = i+1;
          this->buffer_send[4] = 0x00;
          offset = 5;
          //options
          this->buffer_send[offset] = (i==outputs_size-1)? 0x00:0x80 ;
          offset += 1;
          if (found) {
            //Aout
            memmove(this->buffer_send+offset, outKeys.Aout.bytes, 32);
            offset+=32;
            //Bout
            memmove(this->buffer_send+offset, outKeys.Bout.bytes, 32);
            offset+=32;
            //AKout
            memmove(this->buffer_send+offset, outKeys.AKout.bytes, 32);
            offset+=32;
          } else {
            // dummy: Aout Bout AKout
            offset += 32*3;
          }
          //C
          memmove(this->buffer_send+offset, data+C_offset,32);
          offset += 32;
          C_offset += 32;
          //k
          memmove(this->buffer_send+offset, data+kv_offset,32);
          offset += 32;
          //v
          kv_offset += 32;
          memmove(this->buffer_send+offset, data+kv_offset,32);
          offset += 32;
          kv_offset += 32;

          this->buffer_send[4] = offset-5;
          this->length_send = offset;
          this->exchange();
          #ifdef DEBUG_HWDEVICE
          hw::ledger::log_hexbuffer("Prehash AKV input", (char*)&this->buffer_recv[64], 3*32);
          #endif
        }

        // ======   C[], message, proof======
        C_offset = kv_offset;
        for (i = 0; i < outputs_size; i++) {
          reset_buffer();

          this->buffer_send[0] = 0x00;
          this->buffer_send[1] = INS_VALIDATE;
          this->buffer_send[2] = 0x03;
          this->buffer_send[3] = i+1;
          this->buffer_send[4] = 0x00;
          offset = 5;
          //options
          this->buffer_send[offset] = 0x80 ;
          offset += 1;
          //C
          memmove(this->buffer_send+offset, data+C_offset,32);
          offset += 32;
          C_offset += 32;

          this->buffer_send[4] = offset-5;
          this->length_send = offset;
          this->exchange();

        }

        reset_buffer();

        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_VALIDATE;
        this->buffer_send[2] = 0x03;
        this->buffer_send[3] = i+1;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] =  0x00;
        offset += 1;
        //message
        memmove(this->buffer_send+offset, hashes[0].bytes,32);
        offset += 32;
        //proof
        memmove(this->buffer_send+offset,  hashes[2].bytes,32);
        offset += 32;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();

        memmove(prehash.bytes, this->buffer_recv,  32);

        #ifdef DEBUG_HWDEVICE
        hw::ledger::check32("mlsag_prehash", "prehash", (char*)prehash_x.bytes, (char*)prehash.bytes);
        #endif

        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }


    bool device_ledger::mlsag_prepare(const rct::key &H, const rct::key &xx,
                                     rct::key &a, rct::key &aG, rct::key &aHP, rct::key &II) {
      lock_device();
      try {
        int offset =0;
        unsigned char options = 0;

        #ifdef DEBUG_HWDEVICE
        const rct::key H_x = H;
        const rct::key xx_x = hw::ledger::decrypt(xx);
        rct::key a_x;
        rct::key aG_x;
        rct::key aHP_x;
        rct::key II_x;
        #endif

        reset_buffer();

        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_MLSAG;
        this->buffer_send[2] = 0x01;
        this->buffer_send[3] = 0x00;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = 0x00;
        offset += 1;
        //value H
        memmove(this->buffer_send+offset, H.bytes, 32);
        offset += 32;
        //mask xin
        memmove(this->buffer_send+offset, xx.bytes, 32);
        offset += 32;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();

        memmove(a.bytes,   &this->buffer_recv[32*0], 32);
        memmove(aG.bytes,  &this->buffer_recv[32*1], 32);
        memmove(aHP.bytes, &this->buffer_recv[32*2], 32);
        memmove(II.bytes,  &this->buffer_recv[32*3], 32);

        #ifdef DEBUG_HWDEVICE
        a_x = hw::ledger::decrypt(a);

        rct::scalarmultBase(aG_x, a_x);
        rct::scalarmultKey(aHP_x, H_x, a_x);
        rct::scalarmultKey(II_x, H_x, xx_x);
        hw::ledger::check32("mlsag_prepare", "AG", (char*)aG_x.bytes, (char*)aG.bytes);
        hw::ledger::check32("mlsag_prepare", "aHP", (char*)aHP_x.bytes, (char*)aHP.bytes);
        hw::ledger::check32("mlsag_prepare", "II", (char*)II_x.bytes, (char*)II.bytes);
        #endif

        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }

    bool device_ledger::mlsag_prepare(rct::key &a, rct::key &aG) {
      lock_device();
      try {
        int offset =0;
        unsigned char options = 0;

        #ifdef DEBUG_HWDEVICE
        rct::key a_x;
        rct::key aG_x;
        #endif

        reset_buffer();

        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_MLSAG;
        this->buffer_send[2] = 0x01;
        this->buffer_send[3] = 0x00;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = 0x00;
        offset += 1;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();

        memmove(a.bytes,   &this->buffer_recv[32*0], 32);
        memmove(aG.bytes,  &this->buffer_recv[32*1], 32);

        #ifdef DEBUG_HWDEVICE
        a_x = hw::ledger::decrypt(a);
        rct::scalarmultBase(aG_x, a_x);
        hw::ledger::check32("mlsag_prepare", "AG", (char*)aG_x.bytes, (char*)aG.bytes);
        #endif

        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }

    bool device_ledger::mlsag_hash(const rct::keyV &long_message, rct::key &c) {
      lock_device();
      try {
        int offset =0;
        unsigned char options = 0;
        size_t cnt;

        #ifdef DEBUG_HWDEVICE
        const rct::keyV long_message_x = long_message;
        rct::key c_x;
        this->controle_device.mlsag_hash(long_message_x, c_x);
        #endif

        cnt = long_message.size();
        for (size_t i = 0; i<cnt; i++) {
          reset_buffer();

          this->buffer_send[0] = 0x00;
          this->buffer_send[1] = INS_MLSAG;
          this->buffer_send[2] = 0x02;
          this->buffer_send[3] = i+1;
          this->buffer_send[4] = 0x00;
          offset = 5;
          //options
          this->buffer_send[offset] =
              (i==(cnt-1))?0x00:0x80;  //last
          offset += 1;
          //msg part
          memmove(this->buffer_send+offset, long_message[i].bytes, 32);
          offset += 32;

          this->buffer_send[4] = offset-5;
          this->length_send = offset;
          this->exchange();
        }

        memmove(c.bytes, &this->buffer_recv[0], 32);

        #ifdef DEBUG_HWDEVICE
        hw::ledger::check32("mlsag_hash", "c", (char*)c_x.bytes, (char*)c.bytes);
        #endif

        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;

    }

    bool device_ledger::mlsag_sign(const rct::key &c, const rct::keyV &xx, const rct::keyV &alpha, const size_t rows, const size_t dsRows, rct::keyV &ss) {
      lock_device();
      try {
        int offset =0;
        unsigned char options = 0;

        CHECK_AND_ASSERT_THROW_MES(dsRows<=rows, "dsRows greater than rows");
        CHECK_AND_ASSERT_THROW_MES(xx.size() == rows, "xx size does not match rows");
        CHECK_AND_ASSERT_THROW_MES(alpha.size() == rows, "alpha size does not match rows");
        CHECK_AND_ASSERT_THROW_MES(ss.size() == rows, "ss size does not match rows");

        #ifdef DEBUG_HWDEVICE
        const rct::key c_x      = c;
        const rct::keyV xx_x    = hw::ledger::decrypt(xx);
        const rct::keyV alpha_x = hw::ledger::decrypt(alpha);
        const int rows_x        = rows;
        const int dsRows_x      = dsRows;
        rct::keyV ss_x(ss.size());
        this->controle_device.mlsag_sign(c_x, xx_x, alpha_x, rows_x, dsRows_x, ss_x);
        #endif

        for (size_t j = 0; j < dsRows; j++) {
          reset_buffer();

          this->buffer_send[0] = 0x00;
          this->buffer_send[1] = INS_MLSAG;
          this->buffer_send[2] = 0x03;
          this->buffer_send[3] = j+1;
          this->buffer_send[4] = 0x00;
          offset = 5;
          //options
          this->buffer_send[offset] = 0x00;
          if (j==(dsRows-1)) {
            this->buffer_send[offset]  |= 0x80;  //last
          }
          offset += 1;
          //xx
          memmove(this->buffer_send+offset, xx[j].bytes, 32);
          offset += 32;
          //alpa
          memmove(this->buffer_send+offset, alpha[j].bytes, 32);
          offset += 32;

          this->buffer_send[4] = offset-5;
          this->length_send = offset;
          this->exchange();

          //ss
          memmove(ss[j].bytes, &this->buffer_recv[0], 32);
        }

        for (size_t j = dsRows; j < rows; j++) {
          sc_mulsub(ss[j].bytes, c.bytes, xx[j].bytes, alpha[j].bytes);
        }

        #ifdef DEBUG_HWDEVICE
        for (size_t j = 0; j < rows; j++) {
           hw::ledger::check32("mlsag_sign", "ss["+std::to_string(j)+"]", (char*)ss_x[j].bytes, (char*)ss[j].bytes);
        }
        #endif

        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }

    bool device_ledger::close_tx() {
      lock_device();
      try {
        int offset =0;
        unsigned char options = 0;

        reset_buffer();

        this->buffer_send[0] = 0x00;
        this->buffer_send[1] = INS_CLOSE_TX;
        this->buffer_send[2] = 0x00;
        this->buffer_send[3] = 0x00;
        this->buffer_send[4] = 0x00;
        offset = 5;
        //options
        this->buffer_send[offset] = 0x00;
        offset += 1;

        this->buffer_send[4] = offset-5;
        this->length_send = offset;
        this->exchange();

        unlock_tx();
        unlock_device();
      }catch (...)  {
        unlock_device();
        throw;
      }
      return true;
    }

    /* ---------------------------------------------------------- */

    static device_ledger *legder_device = NULL;
    void register_all(std::map<std::string, std::unique_ptr<device>> &registry) {
      if (!legder_device) {
        legder_device = new device_ledger();
        legder_device->set_name("Ledger");
      }
      registry.insert(std::make_pair("Ledger", std::unique_ptr<device>(legder_device)));
    }
  
  #else //WITH_DEVICE_LEDGER
  
    void register_all(std::map<std::string, std::unique_ptr<device>> &registry) {
    }

  #endif //WITH_DEVICE_LEDGER

  }
}

